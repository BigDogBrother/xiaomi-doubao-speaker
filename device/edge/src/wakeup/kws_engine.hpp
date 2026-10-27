#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace xiaoai_plus::wakeup {

struct KwsHit {
  std::string keyword;
  std::string source{"raw"};
};

class IKwsEngine {
 public:
  virtual ~IKwsEngine() = default;
  virtual std::optional<KwsHit> AcceptPcm16(const uint8_t* pcm, size_t size_bytes, int sample_rate,
                                            int channels, int bits_per_sample) = 0;
  virtual void Reset() = 0;
  virtual std::optional<KwsHit> AcceptFilteredPcm16(const uint8_t*, size_t, int, int, int) {
    return std::nullopt;
  }
};

}  // namespace xiaoai_plus::wakeup
