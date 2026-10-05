#include <gtest/gtest.h>

#include <ostream>
#include <string>
#include <type_traits>

#include "azookey/tsf/TypoCorrectionTracker.h"

namespace {
using azookey::tsf::TypoCorrectionTracker;

static_assert(std::is_copy_constructible_v<TypoCorrectionTracker>);
static_assert(std::is_copy_assignable_v<TypoCorrectionTracker>);

TEST(TsfTipTypoCorrectionTrackerTest, PreeditBackspaceReportsTheReadingBeforeCorrection) {
  TypoCorrectionTracker tracker;
  tracker.BeginKey(true, false);
  tracker.BeforeBackspace("こんちには");

  const auto pair = tracker.Commit("こんにちは");
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->wrong_reading, "こんちには");
  EXPECT_EQ(pair->correct_reading, "こんにちは");
}

TEST(TsfTipTypoCorrectionTrackerTest, BackspaceBurstKeepsTheFirstSnapshot) {
  TypoCorrectionTracker tracker;
  tracker.BeforeBackspace("こんちには");
  tracker.BeginKey(true, false);
  tracker.BeforeBackspace("こんちに");
  tracker.BeforeBackspace("こんち");
  tracker.BeginKey(false, false);
  tracker.BeforeBackspace("こんにち");

  const auto pair = tracker.Commit("こんにちは");
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->wrong_reading, "こんちには");
}

TEST(TsfTipTypoCorrectionTrackerTest, EmptyBackspaceDoesNotArmPreeditCorrection) {
  TypoCorrectionTracker tracker;
  tracker.BeforeBackspace("");
  EXPECT_FALSE(tracker.Commit("こんにちは").has_value());
}

TEST(TsfTipTypoCorrectionTrackerTest, FirstBackspaceAfterCommitArmsRetyping) {
  TypoCorrectionTracker tracker;
  EXPECT_FALSE(tracker.Commit("こんちには").has_value());
  tracker.BeginKey(true, true);
  tracker.BeginKey(true, true);
  tracker.BeginKey(false, true);
  tracker.BeforeBackspace("こんにち");

  const auto pair = tracker.Commit("こんにちは");
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->wrong_reading, "こんちには");
  EXPECT_EQ(pair->correct_reading, "こんにちは");
}

TEST(TsfTipTypoCorrectionTrackerTest, OrdinaryFirstKeyClosesRetypingWindow) {
  TypoCorrectionTracker tracker;
  tracker.Commit("こんちには");
  tracker.BeginKey(false, true);
  tracker.BeginKey(true, true);
  EXPECT_FALSE(tracker.Commit("こんにちは").has_value());
}

TEST(TsfTipTypoCorrectionTrackerTest, FirstBackspaceWithPreeditUsesTheNewPreedit) {
  TypoCorrectionTracker tracker;
  tracker.Commit("こんちには");
  tracker.BeginKey(true, false);
  tracker.BeforeBackspace("かに");

  const auto pair = tracker.Commit("かみ");
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->wrong_reading, "かに");
}

TEST(TsfTipTypoCorrectionTrackerTest, NonemptyFirstPreeditClosesRetypingWindow) {
  TypoCorrectionTracker tracker;
  tracker.Commit("こんちには");
  tracker.BeginKey(true, false);
  tracker.BeginKey(true, true);
  EXPECT_FALSE(tracker.Commit("こんにちは").has_value());
}

TEST(TsfTipTypoCorrectionTrackerTest, EachCommitReplacesAndRearmsTheRetypingWindow) {
  TypoCorrectionTracker tracker;
  tracker.Commit("こんちには");
  tracker.BeginKey(false, true);
  tracker.Commit("かに");
  tracker.BeginKey(true, true);

  const auto pair = tracker.Commit("かみ");
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->wrong_reading, "かに");
  tracker.BeginKey(true, true);
  const auto next_pair = tracker.Commit("かめ");
  ASSERT_TRUE(next_pair.has_value());
  EXPECT_EQ(next_pair->wrong_reading, "かみ");
}

TEST(TsfTipTypoCorrectionTrackerTest, RejectedPairDoesNotLeakIntoTheNextCommit) {
  TypoCorrectionTracker tracker;
  tracker.BeforeBackspace("こんちには");
  EXPECT_FALSE(tracker.Commit("").has_value());
  tracker.BeginKey(true, true);
  EXPECT_FALSE(tracker.Commit("こんにちは").has_value());
}

TEST(TsfTipTypoCorrectionTrackerTest, ResetClearsPendingCorrection) {
  TypoCorrectionTracker tracker;
  tracker.BeforeBackspace("こんちには");
  tracker.Reset();
  EXPECT_FALSE(tracker.Commit("こんにちは").has_value());
}

TEST(TsfTipTypoCorrectionTrackerTest, ResetClearsCommittedReadingAndWindow) {
  TypoCorrectionTracker tracker;
  tracker.Commit("こんちには");
  tracker.Reset();
  tracker.BeginKey(true, true);
  EXPECT_FALSE(tracker.Commit("こんにちは").has_value());
}

TEST(TsfTipTypoCorrectionTrackerTest, CopiedValueRestoresPendingCorrectionAndWindow) {
  TypoCorrectionTracker tracker;
  tracker.Commit("こんちには");
  const auto committed_snapshot = tracker;
  tracker.BeginKey(false, true);
  tracker.BeforeBackspace("かに");
  tracker = committed_snapshot;
  tracker.BeginKey(true, true);
  const auto correction_snapshot = tracker;
  tracker.Commit("かに");
  tracker = correction_snapshot;

  const auto pair = tracker.Commit("こんにちは");
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->wrong_reading, "こんちには");
}

struct PairCase {
  const char* name;
  const char* wrong;
  const char* correct;
  bool accepted;
};

void PrintTo(const PairCase& value, std::ostream* out) { *out << value.name; }

class TsfTipTypoCorrectionFilterTest : public ::testing::TestWithParam<PairCase> {};

TEST_P(TsfTipTypoCorrectionFilterTest, AppliesCodepointLengthAndEditDistanceFilters) {
  const auto& test_case = GetParam();
  TypoCorrectionTracker tracker;
  tracker.BeforeBackspace(test_case.wrong);
  const auto pair = tracker.Commit(test_case.correct);
  ASSERT_EQ(pair.has_value(), test_case.accepted);
  if (pair) {
    EXPECT_EQ(pair->wrong_reading, test_case.wrong);
    EXPECT_EQ(pair->correct_reading, test_case.correct);
  }
}

INSTANTIATE_TEST_SUITE_P(
    TypoPairs, TsfTipTypoCorrectionFilterTest,
    ::testing::Values(PairCase{"EmptyWrong", "", "あい", false},
                      PairCase{"EmptyCorrect", "あい", "", false},
                      PairCase{"Equal", "あい", "あい", false},
                      PairCase{"OneCodepointWrong", "あ", "あい", false},
                      PairCase{"OneCodepointCorrect", "あい", "あ", false},
                      PairCase{"TwoCodepointsOneEdit", "あい", "んい", true},
                      PairCase{"TwoCodepointsTwoEdits", "あい", "うえ", false},
                      PairCase{"ThreeCodepointsTwoEdits", "あいう", "えおう", true},
                      PairCase{"LongerCorrectControlsLimit", "あい", "ういえ", true},
                      PairCase{"Insertion", "あいう", "あいうえ", true},
                      PairCase{"Deletion", "あいうえ", "あいう", true},
                      PairCase{"LengthDifferenceOverLimit", "あい", "あいうえおか", false},
                      PairCase{"SixCodepointsThreeEdits", "あいうえおか", "きくけえおか", true},
                      PairCase{"AbsoluteCapThreeEdits", "あいうえおかきくけ", "さしすせおかきくけ",
                               false},
                      PairCase{"SupplementaryCodepoints", "😀😃", "😀😄", true}),
    [](const ::testing::TestParamInfo<PairCase>& info) { return std::string(info.param.name); });

TEST(TsfTipTypoCorrectionTrackerTest, MaximumReadingLengthMatchesHostBound) {
  for (const size_t length : {64u, 65u}) {
    TypoCorrectionTracker tracker;
    std::string wrong;
    std::string correct = "い";
    for (size_t i = 0; i < length; ++i) wrong += "あ";
    for (size_t i = 1; i < length; ++i) correct += "あ";
    tracker.BeforeBackspace(wrong);
    EXPECT_EQ(tracker.Commit(correct).has_value(), length == 64u);
  }
}

}  // namespace
