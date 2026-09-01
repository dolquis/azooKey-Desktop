#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "ConversionQuality.h"

namespace azookey::bench {
namespace {

TEST(ConversionQualityTest, CountsUnicodeScalarsForWagnerFischerDistance) {
  EXPECT_EQ(LevenshteinDistance(DecodeUtf8CodePoints("日本語"), DecodeUtf8CodePoints("日本")), 1u);
  EXPECT_EQ(LevenshteinDistance(DecodeUtf8CodePoints("ab"), DecodeUtf8CodePoints("ba")), 2u);
  EXPECT_EQ(DecodeUtf8CodePoints("😀").size(), 1u);
}

TEST(ConversionQualityTest, CalculatesKnownCerPairsAndEmptyReferenceConvention) {
  const auto replacement = EvaluateCandidateMatch("日本誤", "日本語", {});
  EXPECT_EQ(replacement.edit_distance, 1u);
  EXPECT_EQ(replacement.reference_length, 3u);
  EXPECT_DOUBLE_EQ(replacement.cer, 1.0 / 3.0);

  EXPECT_DOUBLE_EQ(EvaluateCandidateMatch("", "", {}).cer, 0.0);
  EXPECT_DOUBLE_EQ(EvaluateCandidateMatch("追加", "", {}).cer, 1.0);
}

TEST(ConversionQualityTest, SeparatesCanonicalAndAcceptableMatches) {
  const auto canonical = EvaluateCandidateMatch("交渉する", "交渉する", {"交渉します"});
  EXPECT_TRUE(canonical.exact_match);
  EXPECT_FALSE(canonical.acceptable_match);

  const auto variant = EvaluateCandidateMatch("交渉します", "交渉する", {"交渉します"});
  EXPECT_FALSE(variant.exact_match);
  EXPECT_TRUE(variant.acceptable_match);
}

TEST(ConversionQualityTest, ReportsRawAndNfkcBoundaryIndependently) {
  EXPECT_EQ(NormalizeNfkc("仮名"), "仮名");
  EXPECT_EQ(NormalizeNfkc("かな"), "かな");

  const auto result = EvaluateCandidateMatch("ＡＢＣ１２３", "ABC123", {});
  EXPECT_FALSE(result.exact_match);
  EXPECT_TRUE(result.nfkc_exact_match);
  EXPECT_GT(result.cer, 0.0);
  EXPECT_DOUBLE_EQ(result.nfkc_cer, 0.0);
}

TEST(ConversionQualityTest, RejectsInvalidUtf8) {
  EXPECT_THROW(DecodeUtf8CodePoints(std::string("\xc0\x80", 2)), std::invalid_argument);
}

}  // namespace
}  // namespace azookey::bench
