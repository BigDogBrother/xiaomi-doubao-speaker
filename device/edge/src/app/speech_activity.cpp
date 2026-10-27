#include "app/speech_activity.hpp"
#include <stdexcept>
#include <sherpa-onnx/c-api/cxx-api.h>
namespace xiaoai_plus::app {
struct SpeechActivity::Impl {
  sherpa_onnx::cxx::VoiceActivityDetector detector;
  std::vector<float> samples;
  Impl():detector([]{
    sherpa_onnx::cxx::VadModelConfig c;
    c.silero_vad.model="assets/silero_vad.onnx";
    c.silero_vad.threshold=0.5f;c.silero_vad.min_speech_duration=0.1f;
    c.silero_vad.min_silence_duration=0.3f;c.silero_vad.window_size=512;
    c.silero_vad.max_speech_duration=10.0f;c.sample_rate=16000;c.num_threads=1;
    auto d=sherpa_onnx::cxx::VoiceActivityDetector::Create(c,12.0f);
    if(!d.Get())throw std::runtime_error("speech detector unavailable");
    return d;
  }()){}
};
SpeechActivity::SpeechActivity():impl_(std::make_unique<Impl>()){}
SpeechActivity::~SpeechActivity()=default;
bool SpeechActivity::Feed(const std::vector<uint8_t>& pcm){
  if(pcm.empty()||pcm.size()%2)return false;
  auto* input=reinterpret_cast<const int16_t*>(pcm.data());const size_t n=pcm.size()/2;
  impl_->samples.resize(n);
  for(size_t i=0;i<n;++i)impl_->samples[i]=float(input[i])/32768.0f;
  impl_->detector.AcceptWaveform(impl_->samples.data(),int(n));
  bool active=impl_->detector.IsDetected();
  while(!impl_->detector.IsEmpty())impl_->detector.Pop();
  return active;
}
}
