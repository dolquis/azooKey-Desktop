#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "azookey/core/CalculatorRewriter.h"

namespace {

using azookey::core::ExpandCalculatorCandidates;

std::string CalculatorResult(const std::string& reading) {
  const auto candidates = ExpandCalculatorCandidates(reading);
  return candidates.size() == 2 ? candidates[0].surface : std::string("<none>");
}

}  // namespace

TEST(CalculatorRewriterTest, ReturnsTheResultThenTheExpressionWithAnnotations) {
  const auto candidates = ExpandCalculatorCandidates("1＋2＝");

  ASSERT_EQ(candidates.size(), 2u);
  EXPECT_EQ(candidates[0].surface, "3");
  EXPECT_EQ(candidates[0].description, "計算結果");
  EXPECT_EQ(candidates[0].debug_info, "calculator-rewriter:result");
  EXPECT_EQ(candidates[1].surface, "1+2=3");
  EXPECT_EQ(candidates[1].description, "計算式");
  EXPECT_EQ(candidates[1].debug_info, "calculator-rewriter:expression");
  for (const auto& candidate : candidates) {
    EXPECT_EQ(candidate.reading, "1＋2＝");
    EXPECT_EQ(candidate.source, azookey::core::CandidateSource::Heuristic);
  }
}

TEST(CalculatorRewriterTest, FollowsOperatorPrecedenceParenthesesAndUnaryMinus) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"1＋2＊3＝", "7"},
      {"（1＋2）＊3＝", "9"},
      {"10ー4ー3＝", "3"},
      {"12・4・3＝", "1"},
      {"2＊（3＋（4ー1））＝", "12"},
      {"ー5＋2＝", "-3"},
      {"3＊ー2＝", "-6"},
      {"1ーー5＝", "6"},
      {"（ー5）＊2＝", "-10"},
      {"ー（2＋3）＊2＝", "-10"},
      {"007＋1＝", "8"},
      {"1。5＋1＝", "2.5"},
      {"0.1＋0.2＝", "0.3"},
      // Numpad keys stay half-width (spec §20.6).
      {"1+2=", "3"},
      {"6/4=", "1.5"},
      {"2*3-1=", "5"},
      {"(1+2)*3=", "9"},
      {"1＋2=", "3"},
  };
  for (const auto& [reading, result] : cases)
    EXPECT_EQ(CalculatorResult(reading), result) << reading;
}

TEST(CalculatorRewriterTest, RejectsReadingsOutsideTheGrammar) {
  for (const std::string reading :
       {"",           "＝",       "5＝",      "（5）＝",  "ー5＝",    "ーー5＝",
        "1＋ーー5＝", ".5＋1＝",  "5.＋1＝",  "1＋2",     "1＋2＝3",  "1＝2＋3＝",
        "1＋＝",      "＋1＝",    "1＋2＝＝", "（1＋2＝", "1＋2）＝", "1＋a＝",
        "１＋２＝",   "1 ＋ 2＝", "1＋2。＝", "1・・2＝", "きょう",   "123"}) {
    EXPECT_TRUE(ExpandCalculatorCandidates(reading).empty()) << reading;
  }
}

TEST(CalculatorRewriterTest, RejectsDivisionByZeroAndOverflow) {
  EXPECT_TRUE(ExpandCalculatorCandidates("1・0＝").empty());
  EXPECT_TRUE(ExpandCalculatorCandidates("1・（2ー2）＝").empty());
  EXPECT_TRUE(ExpandCalculatorCandidates("9223372036854775807＋1＝").empty());
  EXPECT_TRUE(ExpandCalculatorCandidates("9223372036854775808ー1＝").empty());
  EXPECT_TRUE(ExpandCalculatorCandidates("4294967296＊4294967296＝").empty());
  EXPECT_EQ(CalculatorResult("9223372036854775807ー1＝"), "9223372036854775806");
  EXPECT_EQ(CalculatorResult("ー9223372036854775807ー1＝"), "-9223372036854775808");
  // The fraction digits multiply the remainder by 10, which overflows here
  // although the value itself fits (spec §20.6).
  EXPECT_TRUE(ExpandCalculatorCandidates("2000000000000000000・3000000000000000001＝").empty());
}

TEST(CalculatorRewriterTest, EnforcesTheLengthAndNestingLimits) {
  // 64 code points: 31 "1＋" pairs, then "1＝".
  std::string at_limit;
  for (int i = 0; i < 31; ++i) at_limit += "1＋";
  at_limit += "1＝";
  EXPECT_EQ(CalculatorResult(at_limit), "32");
  EXPECT_TRUE(ExpandCalculatorCandidates("1" + at_limit).empty());

  auto nested = [](int depth) {
    std::string reading;
    for (int i = 0; i < depth; ++i) reading += "（";
    reading += "1";
    for (int i = 0; i < depth; ++i) reading += "）";
    return reading + "＋1＝";
  };
  EXPECT_EQ(CalculatorResult(nested(16)), "2");
  EXPECT_TRUE(ExpandCalculatorCandidates(nested(17)).empty());
}

TEST(CalculatorRewriterTest, RoundsToTenFractionDigitsAwayFromZero) {
  EXPECT_EQ(CalculatorResult("1・3＝"), "0.3333333333");
  EXPECT_EQ(CalculatorResult("2・3＝"), "0.6666666667");
  EXPECT_EQ(CalculatorResult("1・8＝"), "0.125");
  EXPECT_EQ(CalculatorResult("1000000000・3＝"), "333333333.3333333333");
  EXPECT_EQ(CalculatorResult("ー2・3＝"), "-0.6666666667");
  // The carry reaches the integer part.
  EXPECT_EQ(CalculatorResult("0.99999999999＋0＝"), "1");
  EXPECT_EQ(CalculatorResult("1.999999999995＋1＝"), "3");
  // Trailing zeros go; a value that rounds to zero is written 0.
  EXPECT_EQ(CalculatorResult("1.50＋0＝"), "1.5");
  EXPECT_EQ(CalculatorResult("1・100000000000＝"), "0");
  EXPECT_EQ(CalculatorResult("ー1・100000000000＝"), "0");
  EXPECT_EQ(CalculatorResult("1・20000000000＝"), "0.0000000001");
}

TEST(CalculatorRewriterTest, WritesTheExpressionInAsciiAndKeepsLeadingZeros) {
  const auto candidates = ExpandCalculatorCandidates("（007ー1。5）＊2・4＝");
  ASSERT_EQ(candidates.size(), 2u);
  EXPECT_EQ(candidates[0].surface, "2.75");
  EXPECT_EQ(candidates[1].surface, "(007-1.5)*2/4=2.75");
}
