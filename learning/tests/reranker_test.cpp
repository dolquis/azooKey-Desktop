// Direct unit tests for Reranker. Pins down the behaviors that callers
// (InferenceEngine, TIP) rely on:
//   - null-store passthrough
//   - empty / single-candidate handling
//   - stable sort preserves input order when scores tie
//   - time decay matches exp(-0.15 * days) in LearningStore::Score
//   - learning boost flips top candidate
//
// All tests are platform-neutral (no Windows headers, no IPC).

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "azookey/core/Candidate.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/Reranker.h"

namespace core = azookey::core;
namespace learn = azookey::learning;

namespace {

std::vector<core::Candidate> MakeCandidates(
    std::initializer_list<std::pair<const char*, double>> entries,
    const char* reading) {
  std::vector<core::Candidate> out;
  for (const auto& [surface, score] : entries) {
    out.push_back({surface, reading, score,
                   core::CandidateSource::SystemDictionary, "test"});
  }
  return out;
}

}  // namespace

TEST(RerankerTest, NullStorePassthrough) {
  learn::Reranker reranker(nullptr);
  auto cands = MakeCandidates({{"日本", 0.9}, {"二本", 0.5}}, "にほん");
  auto out = reranker.Apply("にほん", std::move(cands), /*now=*/1000);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].surface, "日本");
  EXPECT_EQ(out[1].surface, "二本");
}

TEST(RerankerTest, EmptyCandidates) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "azookey_reranker_empty.tsv").string();
  std::remove(path.c_str());
  learn::LearningStore store(path);
  learn::Reranker reranker(&store);

  auto out = reranker.Apply("にほん", {}, /*now=*/100);
  EXPECT_TRUE(out.empty());
}

TEST(RerankerTest, StableSortOnTie) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "azookey_reranker_tie.tsv").string();
  std::remove(path.c_str());
  learn::LearningStore store(path);
  learn::Reranker reranker(&store);

  // No learning records → store contributes 0 to every candidate, so input
  // order must be preserved for equal scores.
  auto cands = MakeCandidates({{"A", 1.0}, {"B", 1.0}, {"C", 1.0}}, "x");
  auto out = reranker.Apply("x", std::move(cands), /*now=*/100);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0].surface, "A");
  EXPECT_EQ(out[1].surface, "B");
  EXPECT_EQ(out[2].surface, "C");
}

TEST(RerankerTest, DropsNonFiniteScoresBeforeSorting) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "azookey_reranker_nonfinite.tsv").string();
  std::remove(path.c_str());
  learn::LearningStore store(path);
  learn::Reranker reranker(&store);

  store.Observe("x", "boosted-inf", std::numeric_limits<double>::infinity(), /*now=*/100);

  auto cands = MakeCandidates({
                                  {"finite", 1.0},
                                  {"nan", std::numeric_limits<double>::quiet_NaN()},
                                  {"inf", std::numeric_limits<double>::infinity()},
                                  {"boosted-inf", 1.0},
                              },
                              "x");
  auto out = reranker.Apply("x", std::move(cands), /*now=*/100);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out.front().surface, "finite");

  std::remove(path.c_str());
}

TEST(RerankerTest, LearningBoostFlipsTop) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "azookey_reranker_boost.tsv").string();
  std::remove(path.c_str());
  learn::LearningStore store(path);
  learn::Reranker reranker(&store);

  // Initial top = 日本 (score 1.0 > 0.9).
  auto cands1 = MakeCandidates({{"日本", 1.0}, {"二本", 0.9}}, "にほん");
  auto out1 = reranker.Apply("にほん", std::move(cands1), /*now=*/100);
  EXPECT_EQ(out1.front().surface, "日本");

  // After 2 commits of 二本, boost should flip the top (alpha=1.0 each → +2.0).
  store.Observe("にほん", "二本", /*alpha=*/1.0, /*now=*/100);
  store.Observe("にほん", "二本", /*alpha=*/1.0, /*now=*/100);

  auto cands2 = MakeCandidates({{"日本", 1.0}, {"二本", 0.9}}, "にほん");
  auto out2 = reranker.Apply("にほん", std::move(cands2), /*now=*/100);
  EXPECT_EQ(out2.front().surface, "二本");

  std::remove(path.c_str());
}

TEST(RerankerTest, TimeDecay) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "azookey_reranker_decay.tsv").string();
  std::remove(path.c_str());
  learn::LearningStore store(path);
  learn::Reranker reranker(&store);

  // Observe at t=0 (epoch seconds 1700000000).
  constexpr uint64_t kT0 = 1'700'000'000ULL;
  constexpr uint64_t kOneDay = 60 * 60 * 24;
  store.Observe("にほん", "二本", /*alpha=*/5.0, /*now=*/kT0);

  // Same instant: full weight.
  const double s0 = store.Score("にほん", "二本", kT0);
  EXPECT_NEAR(s0, 5.0, 1e-9);

  // 10 days later: decay = exp(-0.15 * 10) ≈ 0.22313.
  const double s10 = store.Score("にほん", "二本", kT0 + 10 * kOneDay);
  const double expected_s10 = 5.0 * std::exp(-0.15 * 10);
  EXPECT_NEAR(s10, expected_s10, 1e-6);

  // Score is monotonically non-increasing with time.
  const double s1 = store.Score("にほん", "二本", kT0 + 1 * kOneDay);
  const double s5 = store.Score("にほん", "二本", kT0 + 5 * kOneDay);
  EXPECT_GE(s0, s1);
  EXPECT_GE(s1, s5);
  EXPECT_GE(s5, s10);

  // After very long time the decayed score should be ~0 (smaller than a 0.5
  // gap), so the reranker should NOT flip the top candidate anymore.
  auto cands = MakeCandidates({{"日本", 1.0}, {"二本", 0.5}}, "にほん");
  auto out = reranker.Apply("にほん", std::move(cands),
                            /*now=*/kT0 + 365 * kOneDay);
  EXPECT_EQ(out.front().surface, "日本");

  std::remove(path.c_str());
}

TEST(RerankerTest, CorrectionDownweightsRejected) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "azookey_reranker_correction.tsv").string();
  std::remove(path.c_str());
  learn::LearningStore store(path);
  learn::Reranker reranker(&store);

  // Observe rejected once, then ObserveCorrection: rejected weight should
  // clamp to 0 (max(0, weight - alpha)) and selected should be boosted.
  store.Observe("にほん", "日本", /*alpha=*/1.0, /*now=*/100);
  store.ObserveCorrection("にほん", "日本", "二本", /*alpha=*/1.0, /*now=*/100);

  EXPECT_LE(store.Score("にほん", "日本", 100), store.Score("にほん", "二本", 100));

  std::remove(path.c_str());
}
