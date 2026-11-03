#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include "config/config.hpp"
#include "realtime/memory.hpp"
#include <set>

namespace xiaoai_plus::realtime {
class Client {
 public:
  struct Callbacks {
    std::function<void(const std::vector<uint8_t>&)> on_audio;
    std::function<void(bool)> on_set_ai_speaking;
    std::function<void(const std::string&)> on_asr_final;
    std::function<void()> on_user_activity;
    std::function<void(const std::string&)> on_session_closed;
    std::function<void()> on_chat_ended;
    std::function<void(const std::string&)> on_text_done; // Optional in-memory diagnostic callback.
  };
  Client(config::Config cfg, Callbacks callbacks);
  ~Client();
  bool Start();
  void Stop();
  bool StartSession(std::chrono::milliseconds timeout = std::chrono::seconds(10));
  bool FinishSession(std::chrono::milliseconds timeout = std::chrono::seconds(2));
  bool EnqueueAudio(const std::vector<uint8_t>& chunk);
  bool EnqueueAudio(std::vector<uint8_t>&& chunk);
  bool SendSayHello();
  bool SendProbe();
  void CancelResponse();
  bool IsReady() const {return ready_.load() && !closing_.load();}
 private:
  void OnMessage(const ix::WebSocketMessagePtr& message);
  void Send(const nlohmann::json& event);
  void SenderLoop();
  std::string Instructions() const;
  nlohmann::json SessionEvent(const std::string& type);
  void HandleTools(const nlohmann::json& event);
  Memory memory_;
  std::string resume_id_, active_id_;
  std::atomic<bool> turn_valid_{false}, suppress_resume_{false};
  bool other_speaker_{false}, retry_fresh_{false};
  std::set<std::string> handled_calls_;
  config::Config cfg_;
  Callbacks callbacks_;
  std::atomic<bool> running_{false}, ready_{false}, closing_{false}, speaking_{false}, capacity_drop_{false};
  std::mutex state_mu_, send_mu_, audio_mu_;
  std::condition_variable state_cv_;
  std::unique_ptr<ix::WebSocket> ws_;
  std::deque<uint8_t> audio_;
  std::thread sender_;
  std::atomic<bool> sending_{false};
  bool input_announced_{false}, started_during_playback_{false};
  bool drop_echo_response_{false}, awaiting_new_response_{false};
  std::string assistant_text_, input_text_;
  std::chrono::steady_clock::time_point ended_at_{};
};
}  // namespace xiaoai_plus::realtime
