#pragma once

#include <cstdint>
#include <functional>

namespace azookey::tsf {

// Reconnect delay with decorrelated jitter (spec §8.3): each delay is drawn
// uniformly from [min_ms, previous * 3] and capped at max_ms, so TIP instances
// loaded into many host applications spread their retries instead of hitting a
// restarted Host in lockstep. Every delay stays within [min_ms, max_ms].
class IpcReconnectBackoff {
 public:
  // Returns a value in the closed range [lo, hi]. Injected so tests can pin the
  // sequence; the default draws from a per-instance PRNG.
  using UniformFn = std::function<uint32_t(uint32_t lo, uint32_t hi)>;

  IpcReconnectBackoff(uint32_t min_ms, uint32_t max_ms, UniformFn uniform = {});

  // Delay before the next reconnect attempt.
  uint32_t Next();
  // Called once a connection is established; the next outage starts over.
  void Reset();

 private:
  uint32_t min_ms_;
  uint32_t max_ms_;
  uint32_t previous_ms_;
  UniformFn uniform_;
};

}  // namespace azookey::tsf
