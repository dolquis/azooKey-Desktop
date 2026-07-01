#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "azookey/core/NumberRewriter.h"

namespace {

const azookey::core::Candidate* FindCandidate(const std::vector<azookey::core::Candidate>& candidates,
                                              const std::string& surface) {
  const auto it = std::find_if(candidates.begin(), candidates.end(), [&](const azookey::core::Candidate& candidate) {
    return candidate.surface == surface;
  });
  return it == candidates.end() ? nullptr : &*it;
}

void ExpectCandidate(const std::vector<azookey::core::Candidate>& candidates,
                     const std::string& surface,
                     const std::string& description) {
  const auto* candidate = FindCandidate(candidates, surface);
  ASSERT_NE(candidate, nullptr) << surface;
  EXPECT_EQ(candidate->description, description);
  EXPECT_EQ(candidate->reading, "123");
  EXPECT_EQ(candidate->source, azookey::core::CandidateSource::Heuristic);
  EXPECT_NE(candidate->debug_info.find("number-rewriter:"), std::string::npos);
}

bool HasDescription(const std::vector<azookey::core::Candidate>& candidates,
                    const std::string& description) {
  return std::any_of(candidates.begin(), candidates.end(), [&](const azookey::core::Candidate& candidate) {
    return candidate.description == description;
  });
}

}  // namespace

TEST(NumberRewriterTest, ExpandsDecimalReadingIntoAnnotatedVariants) {
  const auto candidates = azookey::core::ExpandNumberCandidates("123");

  ExpectCandidate(candidates, "百二十三", "漢数字");
  ExpectCandidate(candidates, "壱百弐拾参", "大字");
  ExpectCandidate(candidates, "0x7b", "16進数");
  ExpectCandidate(candidates, "0173", "8進数");
  ExpectCandidate(candidates, "0b1111011", "2進数");
  EXPECT_EQ(FindCandidate(candidates, "⑫"), nullptr);
  EXPECT_EQ(FindCandidate(candidates, "ⅻ"), nullptr);
}

TEST(NumberRewriterTest, EmitsRangeLimitedCircledAndRomanNumbers) {
  const auto candidates = azookey::core::ExpandNumberCandidates("12");

  ASSERT_NE(FindCandidate(candidates, "⑫"), nullptr);
  ASSERT_NE(FindCandidate(candidates, "Ⅻ"), nullptr);
  ASSERT_NE(FindCandidate(candidates, "ⅻ"), nullptr);

  const auto circled_upper_bound = azookey::core::ExpandNumberCandidates("50");
  ASSERT_NE(FindCandidate(circled_upper_bound, "㊿"), nullptr);

  const auto roman_out_of_range = azookey::core::ExpandNumberCandidates("13");
  EXPECT_FALSE(HasDescription(roman_out_of_range, "ローマ数字（大文字）"));
  EXPECT_FALSE(HasDescription(roman_out_of_range, "ローマ数字（小文字）"));

  const auto out_of_range = azookey::core::ExpandNumberCandidates("51");
  EXPECT_FALSE(HasDescription(out_of_range, "丸数字"));
}

TEST(NumberRewriterTest, HandlesZeroWithoutDuplicateSurfaces) {
  const auto candidates = azookey::core::ExpandNumberCandidates("0");

  ASSERT_NE(FindCandidate(candidates, "零"), nullptr);
  ASSERT_NE(FindCandidate(candidates, "⓪"), nullptr);
  ASSERT_NE(FindCandidate(candidates, "0x0"), nullptr);
  ASSERT_NE(FindCandidate(candidates, "0b0"), nullptr);
  EXPECT_EQ(FindCandidate(candidates, "0"), nullptr);
}

TEST(NumberRewriterTest, KeepsMixedAndInvalidReadingsUnchangedByReturningNoCandidates) {
  EXPECT_TRUE(azookey::core::ExpandNumberCandidates("").empty());
  EXPECT_TRUE(azookey::core::ExpandNumberCandidates("20世紀").empty());
  EXPECT_TRUE(azookey::core::ExpandNumberCandidates("１２").empty());
}

TEST(NumberRewriterTest, SkipsAllCandidatesOnUint64Overflow) {
  EXPECT_TRUE(azookey::core::ExpandNumberCandidates("18446744073709551616").empty());
}

TEST(NumberRewriterTest, SupportsLargeNumbersWithinJapaneseUnitRange) {
  const auto candidates = azookey::core::ExpandNumberCandidates("1234567890123456789");

  ASSERT_NE(FindCandidate(candidates, "百二十三京四千五百六十七兆八千九百一億二千三百四十五万六千七百八十九"),
            nullptr);
  ASSERT_NE(FindCandidate(candidates, "0x112210f47de98115"), nullptr);
}
