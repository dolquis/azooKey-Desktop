#include "azookey/tsf/IpcReconnectBackoff.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <random>

namespace azookey::tsf {
namespace {
IpcReconnectBackoff::UniformFn DefaultUniform() {
  // Seeded per instance: two TIPs in different processes must not share a
  // sequence, which is the whole point of the jitter.
  std::random_device device;
  const auto seed =
      static_cast<uint64_t>(device()) ^
      static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  auto engine = std::make_shared<std::mt19937>(static_cast<std::mt19937::result_type>(seed));
  return [engine](uint32_t lo, uint32_t hi) {
    return std::uniform_int_distribution<uint32_t>(lo, hi)(*engine);
  };
}
}  // namespace

IpcReconnectBackoff::IpcReconnectBackoff(uint32_t min_ms, uint32_t max_ms, UniformFn uniform)
    : min_ms_(min_ms),
      max_ms_(std::max(min_ms, max_ms)),
      previous_ms_(min_ms),
      uniform_(uniform ? std::move(uniform) : DefaultUniform()) {}

uint32_t IpcReconnectBackoff::Next() {
  const uint64_t widened = static_cast<uint64_t>(previous_ms_) * 3;
  const auto hi = static_cast<uint32_t>(std::min<uint64_t>(widened, max_ms_));
  const uint32_t drawn = uniform_(min_ms_, std::max(min_ms_, hi));
  previous_ms_ = std::clamp(drawn, min_ms_, max_ms_);
  return previous_ms_;
}

void IpcReconnectBackoff::Reset() { previous_ms_ = min_ms_; }

}  // namespace azookey::tsf
