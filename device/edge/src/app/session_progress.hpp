#pragma once
#include <atomic>
#include <cstdint>
namespace xiaoai_plus::app {
// Bound a stalled session without limiting a progressing conversation's age.
class SessionProgress {
 public:
  explicit SessionProgress(int64_t stall_ms=60000):stall_ms_(stall_ms){}
  void Touch(int64_t now){last_.store(now);}
  bool Stalled(int64_t now)const{return now-last_.load()>stall_ms_;}
 private:
  const int64_t stall_ms_;
  std::atomic<int64_t> last_{0};
};
}
