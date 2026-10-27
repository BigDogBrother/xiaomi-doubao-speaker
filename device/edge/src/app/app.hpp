#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/player.hpp"
#include "audio/recorder.hpp"
#include "app/native_lease.hpp"
#include "app/speech_activity.hpp"
#include "app/session_progress.hpp"
#include "config/config.hpp"
#include "audio/aec_webrtc.hpp"
#include "realtime/client.hpp"
#include "wakeup/gate.hpp"
#include "wakeup/local_listener.hpp"
#include "wakeup/trigger.hpp"

namespace xiaoai_plus::app {

enum class Lifecycle {
  kInit,
  kRunning,
  kStopping,
  kStopped,
};

class App {
 public:
  explicit App(config::Config cfg);
  ~App();

  bool Run();
  void Stop();

  Lifecycle State() const;
  bool ProbeWake();

 private:
  static void ConvertS32ToS16(const std::vector<uint8_t>& chunk, std::vector<uint8_t>* out);
  static void DownmixToMono(const std::vector<uint8_t>& chunk, int channels,
                            std::vector<uint8_t>* out);
  static void ExtractChannelS16(const std::vector<uint8_t>& chunk, int channels, int channel,
                                std::vector<uint8_t>* out);

  void OnInputAudio(const std::vector<uint8_t>& chunk);
  void OnAudio(const std::vector<uint8_t>& chunk);
  void OnPlaybackChunkPlayed();
  void OnSetAiSpeaking(bool is_speaking);
  void OnAsrFinal(const std::string& text);
  void OnUserActivity();
  void OnSessionClosed(const std::string& reason);
  void OnChatEnded();
  void TryFinalizeFarewell();
  void ResetFarewellStateLocked();
  void BeginFarewellStateLocked();

  void OnArm(const std::string& reason);

  void StartWelcomeTimer();
  void CancelWelcomeTimer();
  void FinishSessionAndDisarm(const std::string& reason);
  void InterruptPlayback();
  void Monitor();

  config::Config cfg_;

  mutable std::mutex state_mu_;
  Lifecycle lifecycle_{Lifecycle::kInit};

  std::unique_ptr<audio::ArecordRecorder> recorder_;
  std::unique_ptr<audio::AplayPlayer> player_;
  std::unique_ptr<realtime::Client> client_;
  std::unique_ptr<dsp::AecWebrtc> aec_;
  std::unique_ptr<dsp::AecWebrtc> wake_aec_;

  std::unique_ptr<wakeup::Gate> gate_;
  std::unique_ptr<wakeup::Trigger> trigger_;
  std::unique_ptr<wakeup::LocalListener> kws_listener_;
  std::unique_ptr<NativeLease> lease_;
  std::unique_ptr<SpeechActivity> speech_;
  SessionProgress progress_;

  std::mutex mu_;
  bool is_ai_speaking_{false};
  bool greeting_playing_{false};
  bool terminal_session_{false};
  std::string terminal_reason_;
  bool farewell_pending_{false};
  bool farewell_chat_ended_{false};
  bool farewell_tts_started_{false};
  size_t pending_playback_chunks_{0};

  // Reused capture-thread buffers to reduce per-frame allocations.
  std::vector<uint8_t> s16_buf_;
  std::vector<uint8_t> capture_mono_buf_;
  std::vector<uint8_t> kws_mono_buf_;
  std::vector<uint8_t> reverse_ref_buf_;
  float highpass_prev_in_{0}, highpass_prev_out_{0};

  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  bool observe_only_{false};
  bool diagnostics_{false};
  bool dual_wake_{false};
  std::atomic<int64_t> native_until_ms_{0}, session_start_ms_{0};
  std::atomic<int64_t> last_output_end_ms_{0};
  std::atomic<bool> previous_voice_{false};
  std::thread monitor_thread_, arm_thread_;
  std::mutex arm_thread_mu_;
  std::mutex run_mu_;
  std::condition_variable run_cv_;
  std::thread welcome_thread_;
  std::mutex welcome_thread_mu_;
  std::atomic<uint64_t> welcome_epoch_{0};
  std::mutex welcome_mu_;
  std::condition_variable welcome_cv_;
  bool welcome_cancelled_{false};
};

}  // namespace xiaoai_plus::app
