#include "audio/player.hpp"

#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "common/log.hpp"

namespace xiaoai_plus::audio {

namespace {
const auto kLog = xiaoai_plus::GetLogger("player");
}  // namespace

AplayPlayer::AplayPlayer(config::Audio cfg, int queue_frames)
    : cfg_(std::move(cfg)), queue_frames_(queue_frames > 0 ? queue_frames : 128) {}

AplayPlayer::~AplayPlayer() { Close(); }

bool AplayPlayer::Start() {
  if (running_.exchange(true)) {
    return true;
  }

  try {
    thread_ = std::thread([this]() { WriteLoop(); });
  } catch (const std::system_error& e) {
    kLog->error("player thread creation failed: {}", e.what());
    running_.store(false);
    return false;
  }
  kLog->info("player started");
  return true;
}

bool AplayPlayer::Play(const std::vector<uint8_t>& chunk) {
  if (chunk.empty()) {
    return true;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (queued_bytes_ + chunk.size() > 120u * 48000u) {
    return false;
  }
  queue_.push_back(chunk);
  queued_bytes_ += chunk.size();
  cv_.notify_one();
  return true;
}

void AplayPlayer::Interrupt() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.clear();
    queued_bytes_ = 0;
  }
  std::lock_guard<std::mutex> pipe_lock(pipe_mu_);
  ClosePipeLocked(true);
}

void AplayPlayer::SetOnChunkPlayed(std::function<void(const std::vector<uint8_t>&)> cb) {
  std::lock_guard<std::mutex> lock(cb_mu_);
  on_chunk_played_ = std::move(cb);
}

bool AplayPlayer::OpenPipeLocked() {
  if (aplay_fd_ >= 0) {
    return true;
  }

  std::string fmt = "S" + std::to_string(cfg_.bits_per_sample) + "_LE";
  std::string rate = std::to_string(cfg_.sample_rate);
  std::string channels = std::to_string(cfg_.channels);
  std::string buffer_size = std::to_string(cfg_.buffer_size);
  std::string period_size = std::to_string(cfg_.period_size);

  std::vector<char*> argv = {
      const_cast<char*>("aplay"),
      const_cast<char*>("--quiet"),
      const_cast<char*>("-t"),           const_cast<char*>("raw"),
      const_cast<char*>("-D"),           const_cast<char*>(cfg_.output_device.c_str()),
      const_cast<char*>("-f"),           const_cast<char*>(fmt.c_str()),
      const_cast<char*>("-r"),           const_cast<char*>(rate.c_str()),
      const_cast<char*>("-c"),           const_cast<char*>(channels.c_str()),
      const_cast<char*>("--buffer-size"), const_cast<char*>(buffer_size.c_str()),
      const_cast<char*>("--period-size"), const_cast<char*>(period_size.c_str()),
      const_cast<char*>("-"),
      nullptr,
  };

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    kLog->error("player pipe() failed");
    return false;
  }
  // Limit queued kernel audio to ~85 ms at 24 kHz. This makes playback-end
  // timing meaningful and lets a real user interruption stop promptly.
  constexpr int kSetPipeSize = 1031; // Linux F_SETPIPE_SZ
  if (fcntl(pipefd[1], kSetPipeSize, 4096) < 0) {
    close(pipefd[0]);close(pipefd[1]);
    kLog->error("player pipe size setup failed");return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    kLog->error("player fork() failed");
    return false;
  }

  if (pid == 0) {
    // child: wire read-end to stdin, exec aplay
    close(pipefd[1]);
    if (dup2(pipefd[0], STDIN_FILENO) < 0) { _exit(1); }
    close(pipefd[0]);
    execvp("aplay", argv.data());
    _exit(1);
  }

  // parent
  close(pipefd[0]);
  aplay_pid_ = pid;
  aplay_fd_ = pipefd[1];
  return true;
}

void AplayPlayer::ClosePipeLocked(bool kill_now) {
  const pid_t pid = aplay_pid_;
  const int fd = aplay_fd_;
  aplay_pid_ = -1;
  aplay_fd_ = -1;

  if (pid > 0 && kill_now) {
    kill(pid, SIGKILL);
  }
  if (fd >= 0) {
    close(fd);
  }
  if (pid > 0) {
    waitpid(pid, nullptr, 0);
  }
}

void AplayPlayer::WriteLoop() {
  const auto idle_timeout = std::chrono::milliseconds(1200);
  while (running_.load()) {
    std::vector<uint8_t> chunk;
    bool timed_out = false;
    {
      std::unique_lock<std::mutex> lock(mu_);
      if (!cv_.wait_for(lock, idle_timeout, [this]() { return !running_.load() || !queue_.empty(); })) {
        timed_out = true;
      }
      if (!running_.load()) {
        break;
      }
      if (timed_out && queue_.empty()) {
        lock.unlock();
        std::lock_guard<std::mutex> pipe_lock(pipe_mu_);
        ClosePipeLocked(false);
        continue;
      }
      chunk = std::move(queue_.front());
      queue_.pop_front();
      queued_bytes_ -= chunk.size();
    }

    if (chunk.empty()) {
      continue;
    }

    bool write_ok = false;
    {
      std::lock_guard<std::mutex> pipe_lock(pipe_mu_);
      if (!OpenPipeLocked()) {
        continue;
      }
      size_t written = 0;
      while (written < chunk.size()) {
        const ssize_t n = write(aplay_fd_,chunk.data()+written,chunk.size()-written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        written += static_cast<size_t>(n);
      }
      if (written != chunk.size()) {
        kLog->warn("player write failed (written={}/{})", written, chunk.size());
        ClosePipeLocked(false);
      } else {
        write_ok = true;
      }
    }
    if (!write_ok) {
      continue;
    }

    std::function<void(const std::vector<uint8_t>&)> cb;
    {
      std::lock_guard<std::mutex> lock(cb_mu_);
      cb = on_chunk_played_;
    }
    if (cb) {
      cb(chunk);
    }
  }
}

void AplayPlayer::Close() {
  if (!running_.exchange(false)) {
    return;
  }

  cv_.notify_all();

  if (thread_.joinable()) {
    thread_.join();
  }

  {
    std::lock_guard<std::mutex> pipe_lock(pipe_mu_);
    ClosePipeLocked(true);
  }

  std::lock_guard<std::mutex> lock(mu_);
  queue_.clear();
  queued_bytes_ = 0;
  kLog->info("player stopped");
}

}  // namespace xiaoai_plus::audio
