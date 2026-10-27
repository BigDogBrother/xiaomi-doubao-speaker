#pragma once
#include <cstdint>
#include <memory>
#include <vector>
namespace xiaoai_plus::app {
class SpeechActivity {
 public:
  SpeechActivity();
  ~SpeechActivity();
  bool Feed(const std::vector<uint8_t>& pcm);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
