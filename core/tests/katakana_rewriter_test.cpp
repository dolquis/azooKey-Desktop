#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "azookey/core/KatakanaRewriter.h"

namespace {

void ExpectCandidate(const azookey::core::Candidate& candidate, const std::string& reading,
                     const std::string& surface, const std::string& description,
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
  // U+304B + U+3099: spec §18.2 excludes decomposed dakuten. Precomposed U+304C is accepted.
  constexpr const char* kDecomposedGa = "\xE3\x81\x8B\xE3\x82\x99";
  for (const std::string reading :
       {"", "ー", "愛してる", "カタカナ", "abc", "あ い", "あ。", kDecomposedGa, "ゝ", "ゞ"}) {
    EXPECT_TRUE(azookey::core::ExpandKatakanaCandidates(reading).empty()) << reading;
  }
}

TEST(KatakanaRewriterTest, RejectsInvalidUtf8AfterValidHiragana) {
  for (const std::string invalid :
       {"\x80", "\xc2", "\xe3\x81", "\xf0\x9f\x98", "\xc0\x80", "\xe0\x80\x80", "\xf0\x80\x80\x80",
        "\xed\xa0\x80", "\xf4\x90\x80\x80"}) {
    EXPECT_TRUE(azookey::core::ExpandKatakanaCandidates("あ" + invalid).empty());
  }
}
