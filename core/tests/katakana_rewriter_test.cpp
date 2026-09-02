#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "azookey/core/KatakanaRewriter.h"

namespace {

void ExpectCandidate(const azookey::core::Candidate& candidate,
                     const std::string& reading,
                     const std::string& surface,
                     const std::string& description,
                     const std::string& debug_tag) {
  EXPECT_EQ(candidate.reading, reading);
  EXPECT_EQ(candidate.surface, surface);
  EXPECT_EQ(candidate.description, description);
  EXPECT_EQ(candidate.source, azookey::core::CandidateSource::Heuristic);
  EXPECT_EQ(candidate.debug_info, "katakana-rewriter:" + debug_tag);
}

}  // namespace

TEST(KatakanaRewriterTest, ExpandsPureHiraganaInFullWidthThenHalfWidthOrder) {
  const auto candidates = azookey::core::ExpandKatakanaCandidates("あっぷる");

  ASSERT_EQ(candidates.size(), 2u);
  ExpectCandidate(candidates[0], "あっぷる", "アップル", "全角カタカナ", "fullwidth");
  ExpectCandidate(candidates[1], "あっぷる", "ｱｯﾌﾟﾙ", "半角カタカナ", "halfwidth");
}

TEST(KatakanaRewriterTest, DecomposesVoicedAndSemiVoicedKanaAndMapsWoAndLongMark) {
  const auto candidates = azookey::core::ExpandKatakanaCandidates("がぱゔをー");

  ASSERT_EQ(candidates.size(), 2u);
  ExpectCandidate(candidates[0], "がぱゔをー", "ガパヴヲー", "全角カタカナ", "fullwidth");
  ExpectCandidate(candidates[1], "がぱゔをー", "ｶﾞﾊﾟｳﾞｦｰ", "半角カタカナ", "halfwidth");
}

TEST(KatakanaRewriterTest, KeepsFullWidthAndDropsHalfWidthWhenAnyKanaHasNoMapping) {
  for (const std::string reading : {"ゐ", "ゑ", "ゎ", "ゕ", "ゖ", "あゐ"}) {
    const auto candidates = azookey::core::ExpandKatakanaCandidates(reading);
    ASSERT_EQ(candidates.size(), 1u) << reading;
    EXPECT_EQ(candidates[0].description, "全角カタカナ");
  }
}

TEST(KatakanaRewriterTest, SupportsTheFullHiraganaRangeForFullWidthConversion) {
  const auto candidates = azookey::core::ExpandKatakanaCandidates("ぁあぃいぅうぇえぉおゔゕゖ");

  ASSERT_FALSE(candidates.empty());
  EXPECT_EQ(candidates[0].surface, "ァアィイゥウェエォオヴヵヶ");
}

TEST(KatakanaRewriterTest, RejectsAnythingOutsidePurePrecomposedHiraganaAndLongMark) {
  for (const std::string reading : {"", "ー", "愛してる", "カタカナ", "abc", "あ い",
                                    "あ。", "が", "ゝ", "ゞ"}) {
    EXPECT_TRUE(azookey::core::ExpandKatakanaCandidates(reading).empty()) << reading;
  }
}
