#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>
#include <vector>

#include "azookey/core/PunctuationRules.h"

namespace {
using namespace azookey::core;

const PunctuationRule* Find(const PunctuationRules& rules, PunctuationKind kind,
                            std::string_view match) {
  const auto& entries = rules.rules();
  const auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& rule) {
    return rule.kind == kind && rule.match == match;
  });
  return it == entries.end() ? nullptr : &*it;
}

TEST(PunctuationRulesTest, BuiltinsSurviveMissingTsvAndRowsOverrideByKindAndMatch) {
  const auto builtins = PunctuationRules::ParseAndMerge("");
  ASSERT_NE(Find(builtins, PunctuationKind::Comma, "ので"), nullptr);
  ASSERT_NE(Find(builtins, PunctuationKind::Period, "です"), nullptr);

  std::vector<size_t> invalid;
  const auto rules = PunctuationRules::ParseAndMerge(
      "\xef\xbb\xbf# version: 1\r\ncomma\tので\t0.75\r\ncomma\tので\t0\r\n"
      "comma\t新規\t0.6\tprev_sem=PlaceName\r\n",
      &invalid);
  EXPECT_TRUE(invalid.empty());
  ASSERT_NE(Find(rules, PunctuationKind::Comma, "ので"), nullptr);
  EXPECT_DOUBLE_EQ(Find(rules, PunctuationKind::Comma, "ので")->base_score, 0.0);
  ASSERT_NE(Find(rules, PunctuationKind::Comma, "新規"), nullptr);
  ASSERT_NE(Find(rules, PunctuationKind::Period, "です"), nullptr);
}

TEST(PunctuationRulesTest, InvalidRowsAreSkippedWithoutLosingFollowingRules) {
  std::vector<size_t> invalid;
  const auto rules = PunctuationRules::ParseAndMerge(
      "unknown\tて\t0.7\ncomma\tて\t1.1\ncomma\t、\t0.5\n"
      "comma\tて\t0.7\tprev_pos=Bad\ncomma\tて\t0.7\tprev_pos=JoshiConj;\n"
      "comma\tて\tnan\ncomma\tあれば\t0.7\tprev_pos!=JoshiCase\n",
      &invalid);
  EXPECT_EQ(invalid, (std::vector<size_t>{1, 2, 3, 4, 5, 6}));
  ASSERT_NE(Find(rules, PunctuationKind::Comma, "あれば"), nullptr);
  EXPECT_DOUBLE_EQ(Find(rules, PunctuationKind::Comma, "て")->base_score, 0.60);
}

TEST(PunctuationRulesTest, UnknownVersionWarnsButStillReadsKnownRows) {
  std::vector<size_t> invalid;
  const auto rules = PunctuationRules::ParseAndMerge("# version: 2\ncomma\t追加\t0.7\n", &invalid);
  EXPECT_EQ(invalid, (std::vector<size_t>{1}));
  ASSERT_NE(Find(rules, PunctuationKind::Comma, "追加"), nullptr);
}

TEST(PunctuationRulesTest, GuardAndUnknownBiasFollowGrammar) {
  const auto rules = PunctuationRules::ParseAndMerge(
      "comma\t条件\t0.7\tprev_pos=JoshiConj;next_head_pos!=HojoYougen;"
      "prev_sem=PlaceName;next_head_sem!=DateTime\n"
      "period\t終止\t1\tsentence_final\ncomma\t無条件\t0.6\n");
  const auto* condition = Find(rules, PunctuationKind::Comma, "条件");
  const auto* final = Find(rules, PunctuationKind::Period, "終止");
  const auto* plain = Find(rules, PunctuationKind::Comma, "無条件");
  ASSERT_NE(condition, nullptr);
  ASSERT_NE(final, nullptr);
  ASSERT_NE(plain, nullptr);
  EXPECT_TRUE(PunctuationRules::MatchesGuard(*condition, SegmentPos::JoshiConj,
                                             SegmentSemantic::PlaceName, SegmentPos::Unknown,
                                             SegmentSemantic::Unknown, false));
  EXPECT_FALSE(PunctuationRules::MatchesGuard(*condition, SegmentPos::Unknown,
                                              SegmentSemantic::PlaceName, SegmentPos::Unknown,
                                              SegmentSemantic::Unknown, false));
  EXPECT_FALSE(PunctuationRules::MatchesGuard(*condition, SegmentPos::JoshiConj,
                                              SegmentSemantic::PlaceName, SegmentPos::HojoYougen,
                                              SegmentSemantic::Unknown, false));
  EXPECT_FALSE(PunctuationRules::MatchesGuard(*condition, SegmentPos::JoshiConj,
                                              SegmentSemantic::Unknown, SegmentPos::Unknown,
                                              SegmentSemantic::Unknown, false));
  EXPECT_FALSE(PunctuationRules::MatchesGuard(*final, SegmentPos::Unknown, SegmentSemantic::Unknown,
                                              SegmentPos::Unknown, SegmentSemantic::Unknown,
                                              false));
  EXPECT_TRUE(PunctuationRules::MatchesGuard(*final, SegmentPos::Unknown, SegmentSemantic::Unknown,
                                             SegmentPos::Unknown, SegmentSemantic::Unknown, true));
  EXPECT_TRUE(PunctuationRules::MatchesGuard(*plain, SegmentPos::Unknown, SegmentSemantic::Unknown,
                                             SegmentPos::Unknown, SegmentSemantic::Unknown, false));
}

}  // namespace
