// Direct unit tests for RequestScheduler. Pins down the primitives the TIP
// staleness check and Host early-return code paths rely on:
//   - NextRequestId monotonic and non-zero starting at 1
//   - Cancel/IsCanceled set semantics with multiple ids
//   - MarkLatest/IsLatest single-id semantics (latest one wins)
//   - thread-safety smoke (multiple threads bumping NextRequestId)
//
// Platform-neutral.

#include <cstdint>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "azookey/host/RequestScheduler.h"

namespace host = azookey::host;

TEST(RequestSchedulerTest, NextRequestIdMonotonic) {
  host::RequestScheduler s;
  EXPECT_EQ(s.NextRequestId(), 1u);
  EXPECT_EQ(s.NextRequestId(), 2u);
  EXPECT_EQ(s.NextRequestId(), 3u);
}

TEST(RequestSchedulerTest, CancelMultiple) {
  host::RequestScheduler s;
  EXPECT_FALSE(s.IsCanceled(1));

  s.Cancel(1);
  s.Cancel(5);
  s.Cancel(100);

  EXPECT_TRUE(s.IsCanceled(1));
  EXPECT_TRUE(s.IsCanceled(5));
  EXPECT_TRUE(s.IsCanceled(100));
  EXPECT_FALSE(s.IsCanceled(2));
  EXPECT_FALSE(s.IsCanceled(99));

  // Cancel is idempotent — second Cancel does not error or unset.
  s.Cancel(1);
  EXPECT_TRUE(s.IsCanceled(1));
}

TEST(RequestSchedulerTest, TrackCancellationProvidesLiveFlagAndCompleteReclaims) {
  host::RequestScheduler s;
  auto flag = s.TrackCancellation(10);
  EXPECT_FALSE(flag->load());
  EXPECT_FALSE(s.IsCanceled(10));

  s.Cancel(10);
  EXPECT_TRUE(flag->load());
  EXPECT_TRUE(s.IsCanceled(10));

  s.CompleteRequest(10);
  EXPECT_FALSE(s.IsCanceled(10));
  EXPECT_TRUE(flag->load());
}

TEST(RequestSchedulerTest, PreCanceledRequestUsesSharedFlagAndCompletes) {
  host::RequestScheduler s;
  s.Cancel(12);
  auto flag = s.TrackCancellation(12);
  EXPECT_TRUE(flag->load());
  EXPECT_TRUE(s.IsCanceled(12));

  s.CompleteRequest(12);
  EXPECT_FALSE(s.IsCanceled(12));
}

TEST(RequestSchedulerTest, SameIdCancelStateLivesUntilAllTrackersComplete) {
  host::RequestScheduler s;
  auto first = s.TrackCancellation(20);
  auto second = s.TrackCancellation(20);
  EXPECT_EQ(first, second);

  s.CompleteRequest(20);
  EXPECT_FALSE(s.IsCanceled(20));

  s.Cancel(20);
  EXPECT_TRUE(first->load());
  EXPECT_TRUE(second->load());
  EXPECT_TRUE(s.IsCanceled(20));

  s.CompleteRequest(20);
  EXPECT_FALSE(s.IsCanceled(20));
  EXPECT_TRUE(second->load());
}

TEST(RequestSchedulerTest, MarkLatestPrunesInactiveOlderCancelsOnly) {
  host::RequestScheduler s;
  s.Cancel(1);
  s.Cancel(5);
  auto active = s.TrackCancellation(6);

  s.MarkLatest(10);
  EXPECT_FALSE(s.IsCanceled(1));
  EXPECT_FALSE(s.IsCanceled(5));
  EXPECT_FALSE(active->load());

  s.Cancel(6);
  EXPECT_TRUE(active->load());
  EXPECT_TRUE(s.IsCanceled(6));
}

TEST(RequestSchedulerTest, OlderUntrackedCancelSurvivesUntilRequestCanTrack) {
  host::RequestScheduler s;
  s.MarkLatest(10);

  s.Cancel(6);
  EXPECT_TRUE(s.IsCanceled(6));
  auto tracked = s.TrackCancellation(6);
  EXPECT_TRUE(tracked->load());

  s.CompleteRequest(6);
  EXPECT_FALSE(s.IsCanceled(6));

  s.Cancel(7);
  EXPECT_TRUE(s.IsCanceled(7));
  s.MarkLatest(11);
  EXPECT_FALSE(s.IsCanceled(7));
}

TEST(RequestSchedulerTest, LatestSingleId) {
  host::RequestScheduler s;
  s.MarkLatest(7);
  EXPECT_TRUE(s.IsLatest(7));
  EXPECT_FALSE(s.IsLatest(6));
  EXPECT_FALSE(s.IsLatest(8));

  s.MarkLatest(9);
  EXPECT_TRUE(s.IsLatest(9));
  EXPECT_FALSE(s.IsLatest(7));

  // MarkLatest with a smaller id replaces — this is by design; callers must
  // pass the freshest id and the scheduler trusts it.
  s.MarkLatest(3);
  EXPECT_TRUE(s.IsLatest(3));
  EXPECT_FALSE(s.IsLatest(9));
}

TEST(RequestSchedulerTest, CancelDoesNotAffectLatest) {
  host::RequestScheduler s;
  s.MarkLatest(42);
  s.Cancel(42);
  // Canceling the latest must NOT clear "latest" flag.
  EXPECT_TRUE(s.IsLatest(42));
  EXPECT_TRUE(s.IsCanceled(42));
}

TEST(RequestSchedulerTest, NextRequestIdThreadSafety) {
  host::RequestScheduler s;
  constexpr int kPerThread = 1000;
  constexpr int kThreads = 4;
  std::vector<std::thread> threads;
  std::vector<std::vector<uint64_t>> results(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      results[t].reserve(kPerThread);
      for (int i = 0; i < kPerThread; ++i) {
        results[t].push_back(s.NextRequestId());
      }
    });
  }
  for (auto& th : threads) th.join();

  // Every id must be unique across threads, and the total count matches.
  std::unordered_set<uint64_t> seen;
  for (const auto& vec : results) {
    for (uint64_t id : vec) {
      EXPECT_TRUE(seen.insert(id).second) << "NextRequestId must produce unique ids";
    }
  }
  EXPECT_EQ(seen.size(), static_cast<size_t>(kThreads * kPerThread));
  // Ids are dense from 1..N.
  for (uint64_t i = 1; i <= static_cast<uint64_t>(kThreads * kPerThread); ++i) {
    EXPECT_EQ(seen.count(i), 1u);
  }
}
