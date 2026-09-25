#include <gtest/gtest.h>

#include <string>

#include "azookey/core/CharacterFormCycle.h"

namespace {
using azookey::core::BuildCharacterFormCycle;
using azookey::core::CharacterFormCycle;
}  // namespace

TEST(CharacterFormCycleTest, BuildsTheSpecifiedFiveFormCycle) {
  const auto cycle = BuildCharacterFormCycle("あした");
  ASSERT_TRUE(cycle);
  EXPECT_EQ(cycle->forms[CharacterFormCycle::kHiragana], "あした");
  EXPECT_EQ(cycle->forms[CharacterFormCycle::kKatakana], "アシタ");
  EXPECT_EQ(cycle->forms[CharacterFormCycle::kHalfwidthKatakana], "ｱｼﾀ");
  EXPECT_EQ(cycle->forms[CharacterFormCycle::kFullwidthAlphanumeric], "ＡＳＨＩＴＡ");
  EXPECT_EQ(cycle->forms[CharacterFormCycle::kAscii], "ASHITA");
}

TEST(CharacterFormCycleTest, HandlesSmallKanaSokuonAndVoicedKana) {
  const auto cycle = BuildCharacterFormCycle("ぎゃっぷ");
  ASSERT_TRUE(cycle);
  EXPECT_EQ(cycle->forms[CharacterFormCycle::kKatakana], "ギャップ");
  EXPECT_EQ(cycle->forms[CharacterFormCycle::kHalfwidthKatakana], "ｷﾞｬｯﾌﾟ");
  EXPECT_EQ(cycle->forms[CharacterFormCycle::kFullwidthAlphanumeric], "ＧＹＡＰＰＵ");
  EXPECT_EQ(cycle->forms[CharacterFormCycle::kAscii], "GYAPPU");
}

TEST(CharacterFormCycleTest, KeepsNUnambiguousBeforeYa) {
  const auto cycle = BuildCharacterFormCycle("しんよう");
  ASSERT_TRUE(cycle);
  EXPECT_EQ(cycle->forms[CharacterFormCycle::kFullwidthAlphanumeric], "ＳＨＩＮ＇ＹＯＵ");
  EXPECT_EQ(cycle->forms[CharacterFormCycle::kAscii], "SHIN'YOU");
}

TEST(CharacterFormCycleTest, RejectsIncompleteOrInvalidInput) {
  constexpr const char* kDecomposedGa = "\xE3\x81\x8B\xE3\x82\x99";
  for (const std::string input : {"", "abc", "あ A", "カタカナ", "っ", "っあ", "っゃ", "ふぁ", "ゐ",
                                  kDecomposedGa, "あ\x80", "あ\xE3\x81"}) {
    EXPECT_FALSE(BuildCharacterFormCycle(input)) << input;
  }
}
