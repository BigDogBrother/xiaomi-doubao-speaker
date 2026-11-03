#include "app/app.hpp"
#include "app/native_event_log.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>
#include <unistd.h>

#include "common/log.hpp"

#include "wakeup/kws_zipformer.hpp"

namespace xiaoai_plus::app {

namespace {

const auto kLog = xiaoai_plus::GetLogger("app");

constexpr int kWakeupTimeoutMs = 10000; // 10 seconds after the final audible sample.
constexpr int kWelcomeResponseTimeoutSec = 8;
constexpr float kKwsThreshold = 0.20f;
constexpr int kMinTriggerIntervalMs = 800;
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct EchoStats {
  double mic_rms{0.0};
  double ref_rms{0.0};
  double corr{0.0};
  double ratio{0.0};
  bool valid_ref{false};
};

EchoStats AnalyzeEcho(const std::vector<uint8_t>& mic_mono,
                      const std::vector<uint8_t>& reverse_ref) {
  EchoStats stats;
  if (mic_mono.empty() || reverse_ref.empty()) {
    return stats;
  }
  const size_t n = std::min(mic_mono.size(), reverse_ref.size()) / sizeof(int16_t);
  if (n == 0) {
    return stats;
  }

  const auto* mic = reinterpret_cast<const int16_t*>(mic_mono.data());
  const auto* ref = reinterpret_cast<const int16_t*>(reverse_ref.data());

  double mic_energy = 0.0;
  double ref_energy = 0.0;
  double cross = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double m = static_cast<double>(mic[i]);
    const double r = static_cast<double>(ref[i]);
    mic_energy += m * m;
    ref_energy += r * r;
    cross += m * r;
  }
  stats.mic_rms = std::sqrt(mic_energy / static_cast<double>(n));
  stats.ref_rms = std::sqrt(ref_energy / static_cast<double>(n));

  // Reference too weak, skip suppression.
  if (ref_energy < 1e7) {
    return stats;
  }
  stats.valid_ref = true;

  stats.corr = std::abs(cross) / std::sqrt((mic_energy + 1.0) * (ref_energy + 1.0));
  stats.ratio = std::sqrt((mic_energy + 1.0) / (ref_energy + 1.0));
  return stats;
}

void ScalePcmS16InPlace(std::vector<uint8_t>* pcm, float gain) {
  if (!pcm || pcm->empty() || pcm->size() % sizeof(int16_t) != 0) {
    return;
  }
  const float safe_gain = std::max(0.0f, gain);
  if (safe_gain >= 0.999f && safe_gain <= 1.001f) {
    return;
  }
  auto* samples = reinterpret_cast<int16_t*>(pcm->data());
  const size_t n = pcm->size() / sizeof(int16_t);
  for (size_t i = 0; i < n; ++i) {
    const float scaled = static_cast<float>(samples[i]) * safe_gain;
    const int32_t v = static_cast<int32_t>(std::round(scaled));
    samples[i] = static_cast<int16_t>(std::max(-32768, std::min(32767, v)));
  }
}

float ComputeUplinkGainWhileAiSpeaking(const EchoStats& stats) {
  // Keep full-duplex: never hard-mute uplink while AI is speaking.
  if (!stats.valid_ref) {
    return 0.30f;
  }

  // Echo-dominant: reference is strong, mic/reference ratio is low, and correlation is non-trivial.
  const bool echo_dominant =
      (stats.ref_rms > 1200.0 && stats.ratio < 0.55 && stats.corr > 0.06);

  // Far-field speech can be low-RMS; avoid near-field-only thresholds.
  // Treat low correlation or higher mic/reference ratio as near-end double-talk evidence.
  const bool has_near_end_speech = (stats.corr < 0.04 || stats.ratio > 0.70);

  if (echo_dominant) {
    // Echo-only: strong suppression; double-talk: allow more uplink for user speech.
    return has_near_end_speech ? 0.34f : 0.006f;
  }

  return has_near_end_speech ? 0.55f : 0.18f;
}

std::string TrimSpace(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::vector<std::string> LoadWakeupKeywords(const std::string& keywords_file) {
  std::ifstream in(keywords_file);
  if (!in.is_open()) {
    throw std::runtime_error("failed to open wakeup.keywords_file: " + keywords_file);
  }

  std::vector<std::string> keywords;
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    line = TrimSpace(std::move(line));
    if (line.empty() || line.front() == '#' || line.front() == ';') {
      continue;
    }

    const auto at = line.rfind('@');
    if (at == std::string::npos) {
      throw std::runtime_error("invalid wakeup.keywords_file line " + std::to_string(lineno) +
                               ": missing '@' label");
    }
    auto keyword = TrimSpace(line.substr(at + 1));
    if (keyword.empty()) {
      throw std::runtime_error("invalid wakeup.keywords_file line " + std::to_string(lineno) +
                               ": empty keyword label");
    }
    keywords.push_back(std::move(keyword));
  }

  std::sort(keywords.begin(), keywords.end());
  keywords.erase(std::unique(keywords.begin(), keywords.end()), keywords.end());
  if (keywords.empty()) {
    throw std::runtime_error("wakeup.keywords_file has no valid keyword label: " +
                             keywords_file);
  }

  return keywords;
}

}  // namespace

App::App(config::Config cfg) : cfg_(std::move(cfg)) {
  observe_only_ = std::getenv("DOUBAO_EDGE_OBSERVE_ONLY") != nullptr;
  diagnostics_ = std::getenv("DOUBAO_EDGE_DIAGNOSTICS") != nullptr;
  dual_wake_ = std::getenv("DOUBAO_EDGE_DUAL_WAKE") != nullptr;
  // Match the tested cloud mic/reference capture route.
  // Use the same synchronized mic/reference route as the cloud service.
  setenv("ALSA_CONFIG_PATH", "/data/doubao-edge/asound.conf", 1);
  cfg_.audio.input_device = "doubao_micref";
  cfg_.audio.output_device = "default";
  cfg_.audio.sample_rate = 16000;
  cfg_.audio.channels = 2;
  cfg_.audio.bits_per_sample = 16;
  // ALSA needs scheduling headroom; callbacks remain 20 ms in the recorder.
  cfg_.audio.buffer_size = 8000;
  cfg_.audio.period_size = 320;

  recorder_ = std::make_unique<audio::ArecordRecorder>(cfg_.audio);

  // Playback is mono S16; capture contains microphone and hardware reference.
  config::Audio player_audio = cfg_.audio;
  player_audio.sample_rate = 24000;
  player_audio.channels = 1;
  player_audio.bits_per_sample = 16;
  player_ = std::make_unique<audio::AplayPlayer>(player_audio, cfg_.budget.output_queue_frames);

  // WebRTC echo and noise suppression process the mono uplink.
  aec_ = std::make_unique<dsp::AecWebrtc>(cfg_.audio.sample_rate, 1);
  if (dual_wake_) wake_aec_ = std::make_unique<dsp::AecWebrtc>(cfg_.audio.sample_rate, 1);

  lease_ = std::make_unique<NativeLease>();
  speech_ = std::make_unique<SpeechActivity>();
  wakeup::Hooks hooks;
  hooks.after_disarm = [this](const std::string&) {
    CancelWelcomeTimer();
    if (client_) client_->FinishSession();
    InterruptPlayback();
    if (lease_) lease_->Release();
    if (access("/tmp/mipns/mute", F_OK) != 0) kLog->info("native_wake_restored");
    else kLog->warn("native_restore_pending");
    const auto guard = NowMs() + 1500;
    auto existing = native_until_ms_.load();
    while (existing < guard && !native_until_ms_.compare_exchange_weak(existing, guard)) {}
  };
  hooks.on_arm = [this](const std::string& reason) {
    std::lock_guard<std::mutex> lock(arm_thread_mu_);
    if (arm_thread_.joinable()) arm_thread_.join();
    arm_thread_ = std::thread([this, reason] { OnArm(reason); });
  };

  gate_ = std::make_unique<wakeup::Gate>(std::chrono::milliseconds(kWakeupTimeoutMs), hooks);

  auto wakeup_keywords = LoadWakeupKeywords(cfg_.wakeup.keywords_file);
  kLog->info("loaded {} wakeup keyword labels from '{}'", wakeup_keywords.size(),
             cfg_.wakeup.keywords_file);
  trigger_ = std::make_unique<wakeup::Trigger>(
      std::move(wakeup_keywords),
      [this](const std::string& keyword) {
        if (keyword == "小爱同学") {native_until_ms_ = NowMs() + 6000; return false;}
        if (keyword == "退出豆包") {
          if (gate_ && gate_->step() == wakeup::Step::kActive) gate_->Disarm("user_exit");
          return false;
        }
        if (NowMs() < native_until_ms_.load()) return false;
        if (observe_only_) {kLog->info("edge_keyword_seen");return false;}
        return gate_->TryWakeup(keyword);
      });

  auto kws_engine = std::make_shared<wakeup::ZipformerKwsEngine>(cfg_.wakeup);
  // Allocate inference buffers before opening the small native shared ring.
  std::vector<uint8_t> warmup(640, 0);
  for (int i=0;i<32;++i) {
    kws_engine->AcceptPcm16(warmup.data(), warmup.size(), 16000, 1, 16);
    if (dual_wake_) kws_engine->AcceptFilteredPcm16(warmup.data(), warmup.size(), 16000, 1, 16);
  }
  kws_engine->Reset();
  wakeup::LocalListener::Config kws_cfg;
  kws_cfg.gate = gate_.get();
  kws_cfg.trigger = trigger_.get();
  kws_cfg.kws_engine = std::move(kws_engine);
  kws_cfg.sample_rate = cfg_.audio.sample_rate;
  kws_cfg.channels = 1;  // KWS receives the microphone channel only
  kws_cfg.bit_depth = 16;  // Capture is already S16
  kws_cfg.min_trigger_interval_ms = kMinTriggerIntervalMs;
  kws_listener_ = std::make_unique<wakeup::LocalListener>(kws_cfg);

  realtime::Client::Callbacks callbacks;
  callbacks.on_audio = [this](const std::vector<uint8_t>& chunk) { OnAudio(chunk); };
  callbacks.on_set_ai_speaking = [this](bool speaking) { OnSetAiSpeaking(speaking); };
  callbacks.on_asr_final = [this](const std::string& text) { OnAsrFinal(text); };
  callbacks.on_user_activity = [this]() { OnUserActivity(); };
  callbacks.on_session_closed = [this](const std::string& reason) { OnSessionClosed(reason); };
  callbacks.on_chat_ended = [this]() { OnChatEnded(); };
  client_ = std::make_unique<realtime::Client>(cfg_, callbacks);

  player_->SetOnChunkPlayed([this](const std::vector<uint8_t>&) {
    OnPlaybackChunkPlayed();
  });
}

App::~App() { Stop(); }

bool App::Run() {
  kLog->info(
      cfg_.audio.input_device, cfg_.audio.sample_rate, cfg_.audio.channels,
      cfg_.audio.bits_per_sample, cfg_.audio.buffer_size, cfg_.audio.period_size,
      kKwsThreshold);

  {
    std::lock_guard<std::mutex> lock(state_mu_);
    lifecycle_ = Lifecycle::kRunning;
  }

  running_.store(true);
  native_until_ms_ = NowMs() + 1500;

  if (!player_->Start()) {
    kLog->error("app start failed: player start");
    running_.store(false);
    return false;
  }
  if (access("/tmp/mipns/mute", F_OK) != 0 && !recorder_->Start([this](const std::vector<uint8_t>& chunk) { OnInputAudio(chunk); })) {
    kLog->error("app start failed: recorder start");
    running_.store(false);
    player_->Close();
    return false;
  }
  if (!client_->Start()) {
    kLog->error("app start failed: realtime client start");
    running_.store(false);
    recorder_->Stop();
    player_->Close();
    return false;
  }
  kLog->info("app components started");
  monitor_thread_ = std::thread(&App::Monitor, this);
  if (!observe_only_) {std::ofstream ready("/tmp/doubao-edge.ready");ready << getpid() << '\n';}

  {
    std::unique_lock<std::mutex> lock(run_mu_);
    run_cv_.wait(lock, [this]() { return !running_.load(); });
  }

  {
    std::lock_guard<std::mutex> lock(state_mu_);
    lifecycle_ = Lifecycle::kStopped;
  }
  return true;
}

void App::Stop() {
  if (stopping_.exchange(true)) {
    return;
  }
  kLog->info("app stopping");
  running_.store(false);
  if (!observe_only_) {
    std::ifstream ready("/tmp/doubao-edge.ready");int owner=0;ready >> owner;
    if(owner==getpid())unlink("/tmp/doubao-edge.ready");
  }
  run_cv_.notify_all();

  {
    std::lock_guard<std::mutex> lock(state_mu_);
    lifecycle_ = Lifecycle::kStopping;
  }

  CancelWelcomeTimer();
  if (recorder_) recorder_->Stop();
  if (kws_listener_) kws_listener_->Close();
  if (client_) client_->Stop();
  if (monitor_thread_.joinable()) monitor_thread_.join();
  {std::lock_guard<std::mutex> lock(arm_thread_mu_); if (arm_thread_.joinable()) arm_thread_.join();}
  if (gate_) {gate_->Disarm("shutdown"); gate_->Close();}
  if (lease_) lease_->Release();
  if (recorder_) recorder_->Stop();
  if (player_) player_->Close();

  {
    std::lock_guard<std::mutex> lock(state_mu_);
    lifecycle_ = Lifecycle::kStopped;
  }
  kLog->info("app stopped");
}

Lifecycle App::State() const {
  std::lock_guard<std::mutex> lock(state_mu_);
  return lifecycle_;
}

bool App::ProbeWake() {return gate_ && gate_->TryWakeup("豆包豆包");}

void App::ConvertS32ToS16(const std::vector<uint8_t>& chunk, std::vector<uint8_t>* out) {
  if (!out) {
    return;
  }
  if (chunk.size() % sizeof(int32_t) != 0) {
    out->assign(chunk.begin(), chunk.end());
    return;
  }
  const size_t n = chunk.size() / sizeof(int32_t);
  const auto* in = reinterpret_cast<const int32_t*>(chunk.data());
  out->resize(n * sizeof(int16_t));
  auto* dst = reinterpret_cast<int16_t*>(out->data());
  for (size_t i = 0; i < n; ++i) {
    // ALSA's S32_LE output maps to S16 by the high 16 bits. Shifting by 8
    // clips this OH2P's real microphone and playback reference.
    int32_t s = in[i] >> 16;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    dst[i] = static_cast<int16_t>(s);
  }
}


void App::DownmixToMono(const std::vector<uint8_t>& chunk, int channels,
                        std::vector<uint8_t>* out) {
  if (!out) {
    return;
  }
  if (channels <= 1 || chunk.empty()) {
    out->assign(chunk.begin(), chunk.end());
    return;
  }
  const size_t stride = static_cast<size_t>(channels);
  if (chunk.size() % (sizeof(int16_t) * stride) != 0) {
    out->assign(chunk.begin(), chunk.end());
    return;
  }
  const auto* in = reinterpret_cast<const int16_t*>(chunk.data());
  const size_t n_frames = chunk.size() / (sizeof(int16_t) * stride);

  // Ch0-Ch2 are mics; Ch3 is playback reference on 4-ch capture.
  const int mic_channels = (channels == 4) ? 3 : std::max(1, channels);

  out->resize(n_frames * sizeof(int16_t));
  auto* dst = reinterpret_cast<int16_t*>(out->data());

  if (mic_channels < 2) {
    for (size_t i = 0; i < n_frames; ++i) {
      dst[i] = in[i * stride];
    }
    return;
  }

  // Delay-and-sum beamforming for 小爱音箱Pro 6-mic circular array.
  // Diameter 7.4cm → max inter-mic distance 7.4cm → max delay ≈ 3 samples at 16kHz.
  // Estimate per-channel delay relative to ch0 via cross-correlation, then align before summing.
  static constexpr int kMaxLag = 3;

  auto xcorr_lag = [&](int ch_a, int ch_b) -> int {
    int best_lag = 0;
    int64_t best = INT64_MIN;
    for (int lag = -kMaxLag; lag <= kMaxLag; ++lag) {
      const size_t t0 = static_cast<size_t>(std::max(0, -lag));
      const size_t t1 = n_frames - static_cast<size_t>(std::max(0, lag));
      int64_t s = 0;
      for (size_t t = t0; t < t1; ++t) {
        s += static_cast<int64_t>(in[t * stride + ch_a]) *
             static_cast<int64_t>(in[(static_cast<int>(t) + lag) * stride + ch_b]);
      }
      if (s > best) {
        best = s;
        best_lag = lag;
      }
    }
    return best_lag;
  };

  // lag_k: ch_k[t + lag_k] aligns with ch0[t].
  const int lag1 = xcorr_lag(0, 1);
  const int lag2 = (mic_channels >= 3) ? xcorr_lag(0, 2) : 0;

  const int n = static_cast<int>(n_frames);
  for (int t = 0; t < n; ++t) {
    int32_t sum = static_cast<int32_t>(in[t * stride]);
    const int t1 = std::max(0, std::min(n - 1, t + lag1));
    sum += static_cast<int32_t>(in[static_cast<size_t>(t1) * stride + 1]);
    if (mic_channels >= 3) {
      const int t2 = std::max(0, std::min(n - 1, t + lag2));
      sum += static_cast<int32_t>(in[static_cast<size_t>(t2) * stride + 2]);
    }
    dst[t] = static_cast<int16_t>(sum / mic_channels);
  }

}

void App::ExtractChannelS16(const std::vector<uint8_t>& chunk, int channels, int channel,
                            std::vector<uint8_t>* out) {
  if (!out) {
    return;
  }
  if (channels <= 1 || channel < 0 || channel >= channels) {
    out->clear();
    return;
  }
  const size_t stride = static_cast<size_t>(channels);
  if (chunk.size() % (sizeof(int16_t) * stride) != 0) {
    out->clear();
    return;
  }
  const auto* in = reinterpret_cast<const int16_t*>(chunk.data());
  const size_t n_frames = chunk.size() / (sizeof(int16_t) * stride);

  out->resize(n_frames * sizeof(int16_t));
  auto* dst = reinterpret_cast<int16_t*>(out->data());
  for (size_t i = 0; i < n_frames; ++i) {
    dst[i] = in[i * stride + static_cast<size_t>(channel)];
  }
}

void App::OnInputAudio(const std::vector<uint8_t>& chunk) {
  if (access("/tmp/mipns/mute", F_OK) == 0 && !lease_->Active() && !lease_->Acquiring()) return;
  s16_buf_ = chunk;
  // AEC uses one stable mic channel for better linear echo model fitting.
  if (cfg_.audio.channels >= 2) {
    ExtractChannelS16(s16_buf_, cfg_.audio.channels, 0, &capture_mono_buf_);
  } else {
    capture_mono_buf_.clear();
  }
  if (capture_mono_buf_.empty()) {
    DownmixToMono(s16_buf_, cfg_.audio.channels, &capture_mono_buf_);
  }
  // Same 80 Hz high-pass used by the known-working cloud capture path.
  auto* near_samples = reinterpret_cast<int16_t*>(capture_mono_buf_.data());
  for (size_t i=0;i<capture_mono_buf_.size()/2;++i) {
    const float x=near_samples[i];
    const float y=x-highpass_prev_in_+0.96907243f*highpass_prev_out_;
    highpass_prev_in_=x;highpass_prev_out_=y;
    near_samples[i]=static_cast<int16_t>(std::max(-32768.0f,std::min(32767.0f,y)));
  }
  // The second routed channel is the hardware playback reference.
  kws_mono_buf_ = capture_mono_buf_;
  ScalePcmS16InPlace(&kws_mono_buf_,24.0f);
  if (cfg_.audio.channels >= 2) {
    ExtractChannelS16(s16_buf_, cfg_.audio.channels, 1, &reverse_ref_buf_);
  } else {
    reverse_ref_buf_.clear();
  }
  if (kws_listener_) {
    std::vector<uint8_t> filtered;
    if (gate_ && gate_->step() == wakeup::Step::kIdle && wake_aec_ && !reverse_ref_buf_.empty()) {
      wake_aec_->AnalyzeReverseStream(reverse_ref_buf_.data(), reverse_ref_buf_.size());
      filtered = wake_aec_->ProcessCaptureStream(capture_mono_buf_.data(), capture_mono_buf_.size());
    }
    kws_listener_->AcceptPcm(kws_mono_buf_, filtered);
  }
  if (!gate_ || gate_->step() != wakeup::Step::kActive || !lease_->Healthy()) {
    return;
  }
  if (aec_ && !reverse_ref_buf_.empty()) {
    aec_->AnalyzeReverseStream(reverse_ref_buf_.data(), reverse_ref_buf_.size());
  }
  if (client_) {
    bool greeting = false;
    bool playback_busy = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      greeting = greeting_playing_;
      playback_busy = greeting_playing_ || is_ai_speaking_ || pending_playback_chunks_ > 0;
    }

    const std::vector<uint8_t>* api_audio = &capture_mono_buf_;
    std::vector<uint8_t> owned_audio;
    if (aec_) {
      auto processed = aec_->ProcessCaptureStream(capture_mono_buf_.data(), capture_mono_buf_.size());
      if (!processed.empty()) {
        owned_audio = std::move(processed);
        api_audio = &owned_audio;
      }
    }
    const bool voice = speech_ && speech_->Feed(*api_audio);
    const bool was_voice=previous_voice_.exchange(voice);
    if (voice && !was_voice && !playback_busy && NowMs()-last_output_end_ms_.load()>1200) {
      if(diagnostics_)kLog->info("local_voice_activity");
      gate_->ReserveForResponse(std::chrono::seconds(20));
    }
    if (greeting) return;

    // Full-duplex continues during playback; linear hardware-reference AEC
    // and the text echo filter suppress speaker feedback without muting speech.

    const bool accepted = client_->EnqueueAudio(*api_audio);
    if (!accepted && client_->IsReady()) gate_->Disarm("input_backlog");
  }
}

void App::OnAudio(const std::vector<uint8_t>& chunk) {
  progress_.Touch(NowMs());
  if (!player_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++pending_playback_chunks_;
  }

  const float gain = cfg_.audio.playback_gain;
  const bool need_gain = !(gain >= 0.999f && gain <= 1.001f);

  bool played;
  if (need_gain) {
    std::vector<uint8_t> amplified = chunk;
    ScalePcmS16InPlace(&amplified, gain);
    played = player_->Play(amplified);
  } else {
    played = player_->Play(chunk);
  }

  if (!played) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (pending_playback_chunks_ > 0) {
        --pending_playback_chunks_;
      }
    }
    kLog->warn("player queue full, drop tts chunk: bytes={}", chunk.size());
    if (client_) client_->CancelResponse();
    TryFinalizeFarewell();
  }
}

void App::OnPlaybackChunkPlayed() {
  progress_.Touch(NowMs());
  bool all_drained = false;
  bool greeting_finished = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (pending_playback_chunks_ > 0) {
      --pending_playback_chunks_;
    }
    if (greeting_playing_) {greeting_playing_ = false;greeting_finished = true;}
    all_drained = pending_playback_chunks_ == 0 && !is_ai_speaking_;
  }
  if (greeting_finished) kLog->info("greeting_playback_finished");
  if (all_drained && gate_) {last_output_end_ms_=NowMs();gate_->SetAiSpeaking(false);}
  TryFinalizeFarewell();
}

void App::OnSetAiSpeaking(bool is_speaking) {
  if(diagnostics_ && is_speaking)kLog->info("response_audio_started");
  bool playback_busy;
  {
    std::lock_guard<std::mutex> lock(mu_);
    is_ai_speaking_ = is_speaking;
    playback_busy = is_speaking || pending_playback_chunks_ > 0 || greeting_playing_;
    if (is_speaking && farewell_pending_) {
      farewell_tts_started_ = true;
    }
  }
  if (gate_) {
    if (!playback_busy) last_output_end_ms_=NowMs();
    gate_->SetAiSpeaking(playback_busy);
  }
  if (!is_speaking) {
    TryFinalizeFarewell();
  }
}

void App::OnAsrFinal(const std::string& text) {
  if (text.find("退出豆包") != std::string::npos || text.find("结束对话") != std::string::npos) {
    if (gate_) gate_->Disarm("user_exit");
    return;
  }
  if (gate_ && gate_->step() == wakeup::Step::kActive) {
    gate_->ReserveForResponse(std::chrono::seconds(20));
  }
  if (text.find("再见") != std::string::npos) {
    std::lock_guard<std::mutex> lock(mu_);
    BeginFarewellStateLocked();
  }
}

void App::OnUserActivity() {
  progress_.Touch(NowMs());
  if(diagnostics_)kLog->info("cloud_user_activity");
  CancelWelcomeTimer();
  if (gate_ && gate_->step() == wakeup::Step::kActive) {
    gate_->RefreshTimeout();
  }

  bool should_interrupt = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    should_interrupt = is_ai_speaking_ || pending_playback_chunks_ > 0;
    if (should_interrupt) {
      is_ai_speaking_ = false;
      // User barge-in cancels pending auto-close on prior farewell turn.
      ResetFarewellStateLocked();
    }
  }
  if (!should_interrupt) {
    if (gate_) gate_->ReserveForResponse(std::chrono::seconds(20));
    return;
  }

  kLog->info("barge-in detected: interrupt current playback");
  if (gate_) {
    gate_->SetAiSpeaking(false);
  }
  InterruptPlayback();
  if (gate_) gate_->ReserveForResponse(std::chrono::seconds(20));
}

void App::OnSessionClosed(const std::string& reason) {
  if (stopping_.load()) {
    return;
  }
  kLog->info("session closed: {}", reason);
  CancelWelcomeTimer();
  bool busy;
  {
    std::lock_guard<std::mutex> lock(mu_);
    ResetFarewellStateLocked();
    terminal_session_ = true;
    terminal_reason_ = reason;
    is_ai_speaking_ = false;
    busy = greeting_playing_ || pending_playback_chunks_ > 0;
  }
  if (!busy) last_output_end_ms_ = NowMs();
  if (gate_) gate_->SetAiSpeaking(busy);
}

void App::OnChatEnded() {
  if (stopping_.load()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!farewell_pending_) {
      return;
    }
    farewell_chat_ended_ = true;
  }
  TryFinalizeFarewell();
}

void App::OnArm(const std::string& reason) {
  kLog->info("gate armed: {}", reason);
  InterruptPlayback();

  {
    std::lock_guard<std::mutex> lock(mu_);
    ResetFarewellStateLocked();
  }

  if (!client_ || !gate_) {
    kLog->error("on_arm skipped: client/gate missing");
    return;
  }
  session_start_ms_ = NowMs();
  progress_.Touch(session_start_ms_.load());
  {
    std::lock_guard<std::mutex> lock(mu_);
    terminal_session_ = false;
    terminal_reason_.clear();
  }
  previous_voice_ = false;
  if (NowMs() < native_until_ms_.load() || !lease_->Acquire()) {
    gate_->Disarm("native_busy"); return;
  }
  if (NowMs() < native_until_ms_.load()) {gate_->Disarm("native_event_race"); return;}
  kLog->info("native_lease_acquired");
  if (!running_ || gate_->step() != wakeup::Step::kActive) {lease_->Release(); return;}
  std::ifstream prompt("/data/doubao-edge/vivi-wake.pcm", std::ios::binary);
  std::vector<uint8_t> cached((std::istreambuf_iterator<char>(prompt)), std::istreambuf_iterator<char>());
  if (cached.empty()) {gate_->Disarm("greeting_missing"); return;}
  {
    std::lock_guard<std::mutex> lock(mu_);
    greeting_playing_ = true; ++pending_playback_chunks_;
  }
  gate_->SetAiSpeaking(true);
  if (!player_->Play(cached)) {gate_->Disarm("greeting_failed"); return;}
  if (!client_->StartSession(std::chrono::seconds(12))) {
    kLog->error("start session failed");
    gate_->Disarm("session_start_failed");
    return;
  }
  kLog->info("doubao_session_ready");
  if (!client_->SendSayHello()) {
    kLog->error("send say_hello failed");
    client_->FinishSession(std::chrono::seconds(2));
    gate_->Disarm("say_hello_failed");
    return;
  }
  if (!running_ || gate_->step() != wakeup::Step::kActive) client_->FinishSession();
}

void App::StartWelcomeTimer() {
  CancelWelcomeTimer();
  const auto epoch = ++welcome_epoch_;
  {
    std::lock_guard<std::mutex> lock(welcome_mu_);
    welcome_cancelled_ = false;
  }

  std::thread timer([this, epoch]() {
    std::unique_lock<std::mutex> lock(welcome_mu_);
    const auto timeout = std::chrono::seconds(kWelcomeResponseTimeoutSec);
    const bool cancelled = welcome_cv_.wait_for(lock, timeout, [this, epoch]() {
      return welcome_cancelled_ || !running_.load() || welcome_epoch_.load() != epoch;
    });
    if (cancelled) {
      return;
    }
    lock.unlock();
    FinishSessionAndDisarm("welcome_timeout");
  });
  std::lock_guard<std::mutex> lock(welcome_thread_mu_);
  welcome_thread_ = std::move(timer);
}

void App::CancelWelcomeTimer() {
  ++welcome_epoch_;
  {
    std::lock_guard<std::mutex> lock(welcome_mu_);
    welcome_cancelled_ = true;
  }
  welcome_cv_.notify_all();
  std::thread timer_to_join;
  {
    std::lock_guard<std::mutex> lock(welcome_thread_mu_);
    if (welcome_thread_.joinable() &&
        welcome_thread_.get_id() != std::this_thread::get_id()) {
      timer_to_join = std::move(welcome_thread_);
    }
  }
  if (timer_to_join.joinable()) {
    timer_to_join.join();
  }
}

void App::FinishSessionAndDisarm(const std::string& reason) {
  kLog->info("finish session and disarm: {}", reason);
  CancelWelcomeTimer();
  InterruptPlayback();
  {
    std::lock_guard<std::mutex> lock(mu_);
    ResetFarewellStateLocked();
  }
  if (client_) {
    client_->FinishSession(std::chrono::seconds(2));
  }
  if (gate_) {
    gate_->Disarm(reason);
  }
}

void App::TryFinalizeFarewell() {
  bool should_close = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (farewell_pending_ && farewell_chat_ended_ && farewell_tts_started_ &&
        !is_ai_speaking_ &&
        pending_playback_chunks_ == 0) {
      should_close = true;
      ResetFarewellStateLocked();
    }
  }

  if (!should_close) {
    return;
  }

  kLog->info("farewell close: playback drained, closing session");
  CancelWelcomeTimer();
  if (client_) client_->FinishSession(std::chrono::seconds(4));
  if (gate_) gate_->Disarm("bye");
}

void App::ResetFarewellStateLocked() {
  farewell_pending_ = false;
  farewell_chat_ended_ = false;
  farewell_tts_started_ = false;
}

void App::BeginFarewellStateLocked() {
  farewell_pending_ = true;
  farewell_chat_ended_ = false;
  farewell_tts_started_ = false;
}

void App::InterruptPlayback() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    pending_playback_chunks_ = 0;
    greeting_playing_ = false;
    is_ai_speaking_ = false;
  }
  if (player_) {
    player_->Interrupt();
  }
}

void App::Monitor() {
  // Only examine event types; never save or print native transcripts.
  NativeEventLog event_file("/tmp/mico_aivs_lab/event.log");
  NativeEventLog instruction_file("/tmp/mico_aivs_lab/instruction.log");
  while (running_) {
    const bool physical_mute = access("/tmp/mipns/mute", F_OK) == 0 && !lease_->Active() && !lease_->Acquiring();
    if (physical_mute && recorder_->IsRunning()) recorder_->Stop();
    else if (!physical_mute && recorder_->IsRunning() && !recorder_->Healthy()) {
      if (gate_ && gate_->step() == wakeup::Step::kActive) gate_->Disarm("capture_lost");
      recorder_->Stop();
    } else if (!physical_mute && !recorder_->IsRunning() && running_) {
      if (!recorder_->Start([this](const std::vector<uint8_t>& chunk) {OnInputAudio(chunk);})) {
        kLog->error("capture_restart_failed");running_=false;run_cv_.notify_all();break;
      }
    }
    for (auto* stream : {&event_file, &instruction_file}) {
      stream->Poll([this](const std::string& line) {
        try {
          const auto first = line.find('{');
          if (first == std::string::npos) return;
          auto e = nlohmann::json::parse(line.substr(first));
          auto h = e.value("header", nlohmann::json::object());
          const auto ns = h.value("namespace", std::string());
          const auto name = h.value("name", std::string());
          const bool native = ns == "SpeechRecognizer" || ns == "SpeechSynthesizer" ||
                              ns == "Nlp" || (ns == "SpeechWakeup" && name == "Wakeup") ||
                              (ns == "Dialog" && name == "Finish");
          if (native) {
            native_until_ms_ = NowMs() + 6000;
            if (lease_->Active() && gate_ && gate_->step() == wakeup::Step::kActive)
              gate_->Disarm("native_event");
          }
        } catch (...) { /* Native log may contain unrelated lines. */ }
      });
    }
    if (gate_ && gate_->step() == wakeup::Step::kActive) {
      if (lease_->Active() && !lease_->Healthy()) gate_->Disarm("lease_lost");
      else if (lease_->Active()) {
        bool terminal, busy;std::string reason;
        {
          std::lock_guard<std::mutex> lock(mu_);
          terminal=terminal_session_;reason=terminal_reason_;
          busy=is_ai_speaking_ || greeting_playing_ || pending_playback_chunks_>0;
        }
        if (terminal && !busy && NowMs()-last_output_end_ms_.load()>250)
          gate_->Disarm(reason);
        else if (progress_.Stalled(NowMs())) gate_->Disarm("session_stalled");
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}

}  // namespace xiaoai_plus::app
