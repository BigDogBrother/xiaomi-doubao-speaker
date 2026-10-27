#include "audio/aec_webrtc.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <webrtc/common.h>
#include <webrtc/modules/audio_processing/include/audio_processing.h>
#include <webrtc/modules/interface/module_common_types.h>
namespace xiaoai_plus::dsp {
struct AecWebrtc::Impl {
  std::unique_ptr<webrtc::AudioProcessing> apm;
  std::vector<int16_t> reverse,capture;
  Impl(int rate,int channels) {
    if(rate!=16000 || channels!=1)throw std::runtime_error("unsupported AEC format");
    webrtc::Config config;
    config.Set<webrtc::ExtendedFilter>(new webrtc::ExtendedFilter(true));
    config.Set<webrtc::DelayAgnostic>(new webrtc::DelayAgnostic(true));
    apm.reset(webrtc::AudioProcessing::Create(config));
    if(!apm || apm->Initialize(16000,16000,16000,webrtc::AudioProcessing::kMono,webrtc::AudioProcessing::kMono,webrtc::AudioProcessing::kMono) ||
       apm->echo_cancellation()->Enable(true) || apm->echo_cancellation()->set_suppression_level(webrtc::EchoCancellation::kHighSuppression) ||
       apm->noise_suppression()->Enable(true) || apm->noise_suppression()->set_level(webrtc::NoiseSuppression::kModerate))
      throw std::runtime_error("AEC initialization failed");
  }
};
AecWebrtc::AecWebrtc(int rate,int channels):impl_(std::make_unique<Impl>(rate,channels)){}
AecWebrtc::~AecWebrtc()=default;
void AecWebrtc::AnalyzeReverseStream(const uint8_t* pcm,size_t size) {
  std::lock_guard<std::mutex> lock(mu_);
  auto* s=reinterpret_cast<const int16_t*>(pcm);
  impl_->reverse.insert(impl_->reverse.end(),s,s+size/2);
}
std::vector<uint8_t> AecWebrtc::ProcessCaptureStream(const uint8_t* pcm,size_t size) {
  std::lock_guard<std::mutex> lock(mu_);
  auto* s=reinterpret_cast<const int16_t*>(pcm);
  impl_->capture.insert(impl_->capture.end(),s,s+size/2);
  std::vector<int16_t> output;
  while(impl_->capture.size()>=160 && impl_->reverse.size()>=160) {
    webrtc::AudioFrame render,capture;
    render.sample_rate_hz_=capture.sample_rate_hz_=16000;
    render.samples_per_channel_=capture.samples_per_channel_=160;
    render.num_channels_=capture.num_channels_=1;
    std::memcpy(render.data_,impl_->reverse.data(),160*sizeof(int16_t));
    for(int i=0;i<160;i++)capture.data_[i]=std::max(-32768,std::min(32767,int(impl_->capture[i])*8));
    if(impl_->apm->AnalyzeReverseStream(&render) || impl_->apm->set_stream_delay_ms(0) || impl_->apm->ProcessStream(&capture))
      throw std::runtime_error("AEC processing failed");
    for(int i=0;i<160;i++)output.push_back(static_cast<int16_t>(std::max(-31128,std::min(31128,int(capture.data_[i])*3))));
    impl_->capture.erase(impl_->capture.begin(),impl_->capture.begin()+160);
    impl_->reverse.erase(impl_->reverse.begin(),impl_->reverse.begin()+160);
  }
  std::vector<uint8_t> result(output.size()*sizeof(int16_t));
  if(!result.empty())std::memcpy(result.data(),output.data(),result.size());
  return result;
}
}
