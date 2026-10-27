#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
namespace xiaoai_plus::app {
// Device-side watchdog owns only its unique native wake lease and player.
class NativeLease {
 public:
  explicit NativeLease(std::string helper = "/tmp/doubao-voice-lease.sh");
  ~NativeLease();
  bool Acquire();
  void Release();
  bool Healthy() const {return active_.load() && !lost_.load();}
  bool Active() const {return active_.load();}
  bool Acquiring() const {return acquiring_.load();}
 private:
  bool Run(const char* command);
  std::string token_;
  std::string helper_;
  std::atomic<bool> active_{false}, lost_{false}, stopping_{false}, acquiring_{false};
  std::thread heartbeat_;
  std::mutex mu_;
};
}
