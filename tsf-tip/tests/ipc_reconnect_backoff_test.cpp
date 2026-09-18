// Pins the jittered reconnect delay of the TIP IPC worker (docs/dev-infrastructure-spec.md
// §8.3, DEV-1174): bounded by the existing 250ms / 3000ms limits, spread rather
// than lockstep, and deterministic once the random source is injected.
#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "azookey/tsf/IpcReconnectBackoff.h"

namespace {
using azookey::tsf::IpcReconnectBackoff;

constexpr uint32_t kMinMs = 250;
constexpr uint32_t kMaxMs = 3000;

TEST(TsfTipIpcReconnectBackoffTest, InjectedSourceSeesTheDecorrelatedJitterRange) {
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  IpcReconnectBackoff backoff(kMinMs, kMaxMs, [&](uint32_t lo, uint32_t hi) {
    ranges.emplace_back(lo, hi);
    return hi;  // Always the upper end: the widest the window can grow.
  });

  EXPECT_EQ(backoff.Next(), 750u);
  EXPECT_EQ(backoff.Next(), 2250u);
  EXPECT_EQ(backoff.Next(), kMaxMs);
  EXPECT_EQ(backoff.Next(), kMaxMs);
  const std::vector<std::pair<uint32_t, uint32_t>> expected = {
      {kMinMs, 750}, {kMinMs, 2250}, {kMinMs, kMaxMs}, {kMinMs, kMaxMs}};
  EXPECT_EQ(ranges, expected);
}

TEST(TsfTipIpcReconnectBackoffTest, LowerEndStaysAtTheMinimum) {
  IpcReconnectBackoff backoff(kMinMs, kMaxMs, [](uint32_t lo, uint32_t) { return lo; });
  for (int i = 0; i < 10; ++i) EXPECT_EQ(backoff.Next(), kMinMs);
}

TEST(TsfTipIpcReconnectBackoffTest, OutOfRangeSourceIsClampedToTheBounds) {
  uint32_t value = 0;
  IpcReconnectBackoff backoff(kMinMs, kMaxMs, [&](uint32_t, uint32_t) { return value; });
  value = 1;
  EXPECT_EQ(backoff.Next(), kMinMs);
  value = 1'000'000;
  EXPECT_EQ(backoff.Next(), kMaxMs);
}

TEST(TsfTipIpcReconnectBackoffTest, ResetStartsTheNextOutageFromTheMinimumWindow) {
  std::vector<uint32_t> upper_bounds;
  IpcReconnectBackoff backoff(kMinMs, kMaxMs, [&](uint32_t, uint32_t hi) {
    upper_bounds.push_back(hi);
    return hi;
  });
  backoff.Next();
  backoff.Next();
  backoff.Reset();
  backoff.Next();
  EXPECT_EQ(upper_bounds, (std::vector<uint32_t>{750, 2250, 750}));
}

TEST(TsfTipIpcReconnectBackoffTest, SeededSourceSpreadsDelaysWithinBounds) {
  std::mt19937 engine(12345);
  IpcReconnectBackoff backoff(kMinMs, kMaxMs, [&](uint32_t lo, uint32_t hi) {
    return std::uniform_int_distribution<uint32_t>(lo, hi)(engine);
  });
  std::set<uint32_t> distinct;
  for (int i = 0; i < 200; ++i) {
    const uint32_t delay = backoff.Next();
    EXPECT_GE(delay, kMinMs);
    EXPECT_LE(delay, kMaxMs);
    distinct.insert(delay);
  }
  EXPECT_GT(distinct.size(), 50u);
}

TEST(TsfTipIpcReconnectBackoffTest, IndependentDefaultInstancesDoNotRetryInLockstep) {
  // Two TIPs in different host applications each own a default instance. With
  // the old doubling they produced identical sequences; with jitter at least
  // one of the first delays differs.
  IpcReconnectBackoff first(kMinMs, kMaxMs);
  IpcReconnectBackoff second(kMinMs, kMaxMs);
  bool differs = false;
  for (int i = 0; i < 16 && !differs; ++i) {
    const uint32_t a = first.Next();
    const uint32_t b = second.Next();
    EXPECT_GE(a, kMinMs);
    EXPECT_LE(a, kMaxMs);
    differs = a != b;
  }
  EXPECT_TRUE(differs);
}
}  // namespace
