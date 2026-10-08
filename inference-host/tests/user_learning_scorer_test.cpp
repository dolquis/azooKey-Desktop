#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../learning/tests/TestByteCrypto.h"
#include "azookey/core/Candidate.h"
#include "azookey/host/UserLearningScorer.h"
#include "azookey/learning/LearningDecay.h"
#include "azookey/learning/LearningStore.h"

namespace {

namespace host = azookey::host;
namespace learning = azookey::learning;
using learning::LearningEventType;

constexpr uint64_t kNow = 2'000'000'000;
constexpr uint64_t kDay = 24 * 60 * 60;
constexpr uint16_t kTechnical = 1U << 8;

host::UserLearningContext Context(const char* app = "") {
  return host::UserLearningContext{"こうしょう", app, kNow};
}

void Commit(learning::LearningStore& store, const char* surface, const char* app, int times,
            uint64_t when = kNow) {
  for (int i = 0; i < times; ++i) {
    store.ObserveEvent({"こうしょう", surface, app, LearningEventType::Commit, ""}, 0.8, when);
  }
}

}  // namespace

TEST(UserLearningScorerTest, ScoreIsLogCommitCountTimesRecency) {
  learning::LearningStore store("unused.tsv", &learning::test::Crypto());
  Commit(store, "交渉", "", 3, kNow - 2 * kDay);
  host::UserLearningScorer scorer(&store, nullptr);
  EXPECT_NEAR(scorer.CalcUserScore(Context(), "交渉"), std::log(4.0) * std::exp(-0.15 * 2), 1e-12);
  EXPECT_DOUBLE_EQ(scorer.CalcUserScore(Context(), "未学習"), 0.0);
}

TEST(UserLearningScorerTest, CategoryHalfLifeModeUsesTheCategoryOfThePair) {
  learning::LearningStore store("unused.tsv", &learning::test::Crypto());
  Commit(store, "交渉", "", 1, kNow - 120 * kDay);
  const auto technical = [](const std::string&, const std::string&) { return kTechnical; };
  host::UserLearningScorer scorer(&store, technical, {host::LearningDecayMode::CategoryHalfLife});
  EXPECT_NEAR(scorer.CalcUserScore(Context(), "交渉"), std::log(2.0) * 0.5, 1e-12);

  // No lookup: the general half life applies.
  host::UserLearningScorer general(&store, nullptr, {host::LearningDecayMode::CategoryHalfLife});
  EXPECT_NEAR(general.CalcUserScore(Context(), "交渉"), std::log(2.0) * std::pow(0.5, 4.0), 1e-12);
}

TEST(UserLearningScorerTest, TypoRowsKeepAtLeastTheTypoHalfLife) {
  learning::LearningStore store("unused.tsv", &learning::test::Crypto());
  store.ObserveEvent({"こうしょう", "交渉", "", LearningEventType::TypoAccept, ""}, 0.8,
                     kNow - 60 * kDay);
  host::UserLearningScorer scorer(&store, nullptr, {host::LearningDecayMode::CategoryHalfLife});
  EXPECT_NEAR(scorer.CalcUserScore(Context(), "交渉"), std::log(2.0) * 0.5, 1e-12);
}

TEST(UserLearningScorerTest, AppProfileWeightFavoursTheSameApp) {
  learning::LearningStore store("unused.tsv", &learning::test::Crypto());
  Commit(store, "交渉", "outlook.exe", 1);
  host::UserLearningScorer scorer(&store, nullptr);
  const double base = std::log(2.0);
  EXPECT_NEAR(scorer.CalcUserScore(Context("C:\\Office\\OUTLOOK.EXE"), "交渉"), base * 1.2, 1e-12);
  EXPECT_NEAR(scorer.CalcUserScore(Context("code.exe"), "交渉"), base * 0.8, 1e-12);
  EXPECT_NEAR(scorer.CalcUserScore(Context(""), "交渉"), base, 1e-12);

  host::UserLearningScorerConfig off;
  off.app_profile_enabled = false;
  host::UserLearningScorer disabled(&store, nullptr, off);
  EXPECT_NEAR(disabled.CalcUserScore(Context("code.exe"), "交渉"), base, 1e-12);
}

TEST(UserLearningScorerTest, NetRejectionsMultiplyThePenalty) {
  learning::LearningStore store("unused.tsv", &learning::test::Crypto());
  Commit(store, "交渉", "", 3);
  host::UserLearningScorer scorer(&store, nullptr);
  const double base = scorer.CalcUserScore(Context(), "交渉");
  store.ObserveEvent({"こうしょう", "交渉", "", LearningEventType::CorrectionReject, ""}, 0.0,
                     kNow);
  EXPECT_NEAR(scorer.CalcUserScore(Context(), "交渉"), base * 0.7, 1e-12);
  // An accept cancels a reject (net_reject = max(0, reject - accept)).
  store.ObserveEvent({"こうしょう", "交渉", "", LearningEventType::CorrectionAccept, ""}, 0.0,
                     kNow);
  EXPECT_NEAR(scorer.CalcUserScore(Context(), "交渉"), std::log(5.0), 1e-12);
  for (int i = 0; i < 5; ++i) {
    store.ObserveEvent({"こうしょう", "交渉", "", LearningEventType::CorrectionReject, ""}, 0.0,
                       kNow);
  }
  EXPECT_DOUBLE_EQ(scorer.CalcUserScore(Context(), "交渉"), 0.0);
}

TEST(UserLearningScorerTest, ScoreSortsCandidatesAndPutsNonFiniteLast) {
  learning::LearningStore store("unused.tsv", &learning::test::Crypto());
  Commit(store, "校章", "", 5);
  host::UserLearningScorer scorer(&store, nullptr);
  std::vector<azookey::core::Candidate> candidates = {
      {"交渉", "こうしょう", 1.0},
      {"不正", "こうしょう", std::numeric_limits<double>::quiet_NaN()},
      {"校章", "こうしょう", 0.5},
      {"高尚", "こうしょう", 0.9},
  };
  scorer.Score(Context(), candidates);
  ASSERT_EQ(candidates.size(), 4u);
  EXPECT_EQ(candidates[0].surface, "校章");
  EXPECT_EQ(candidates[1].surface, "交渉");
  EXPECT_EQ(candidates[2].surface, "高尚");
  EXPECT_EQ(candidates[3].surface, "不正");
}

TEST(UserLearningScorerTest, FailingCategoryLookupScoresZero) {
  learning::LearningStore store("unused.tsv", &learning::test::Crypto());
  Commit(store, "交渉", "", 1);
  const auto failing = [](const std::string&, const std::string&) -> uint16_t {
    throw std::runtime_error("dictionary unavailable");
  };
  host::UserLearningScorer scorer(&store, failing, {host::LearningDecayMode::CategoryHalfLife});
  EXPECT_DOUBLE_EQ(scorer.CalcUserScore(Context(), "交渉"), 0.0);
  host::UserLearningScorer no_store(nullptr, nullptr);
  EXPECT_DOUBLE_EQ(no_store.CalcUserScore(Context(), "交渉"), 0.0);
}
