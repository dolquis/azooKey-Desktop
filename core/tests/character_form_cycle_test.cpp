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

namespace {
using azookey::core::BuildCommitForm;
using azookey::core::CommitForm;
}  // namespace

TEST(CommitFormTest, CommitsTheReadingInEachForm) {
  EXPECT_EQ(BuildCommitForm("かんじ", CommitForm::Hiragana), "かんじ");
  EXPECT_EQ(BuildCommitForm("かんじ", CommitForm::Katakana), "カンジ");
  EXPECT_EQ(BuildCommitForm("かんじ", CommitForm::HalfwidthKatakana), "ｶﾝｼﾞ");
  EXPECT_EQ(BuildCommitForm("かんじ", CommitForm::FullwidthRoman, "kanji"), "ｋａｎｊｉ");
  EXPECT_EQ(BuildCommitForm("かんじ", CommitForm::HalfwidthRoman, "kanji"), "kanji");
}

TEST(CommitFormTest, RomanFormsSpellTheTypedRomajiWithItsCase) {
  EXPECT_EQ(BuildCommitForm("ぴっつぁ", CommitForm::HalfwidthRoman, "PIzza"), "PIzza");
  EXPECT_EQ(BuildCommitForm("ぴっつぁ", CommitForm::FullwidthRoman, "PIzza"), "ＰＩｚｚａ");
  EXPECT_EQ(BuildCommitForm("らーめん、", CommitForm::HalfwidthRoman, "ra-men,"), "ra-men,");
}

TEST(CommitFormTest, KanaFormsKeepDigitsAndSymbolsOfTheReading) {
  EXPECT_EQ(BuildCommitForm("だい3かい", CommitForm::Hiragana), "だい3かい");
  EXPECT_EQ(BuildCommitForm("だい3かい", CommitForm::Katakana), "ダイ3カイ");
  EXPECT_EQ(BuildCommitForm("だい3かい", CommitForm::HalfwidthKatakana), "ﾀﾞｲ3ｶｲ");
  EXPECT_EQ(BuildCommitForm("かっこ「", CommitForm::Katakana), "カッコ「");
}

TEST(CommitFormTest, HalfwidthKatakanaNarrowsOnlyCharactersWithAHalfwidthForm) {
  EXPECT_EQ(BuildCommitForm("＠：＊￥「」、。・ー", CommitForm::HalfwidthKatakana), "@:*¥｢｣､｡･ｰ");
  EXPECT_EQ(BuildCommitForm("’”〜『』", CommitForm::HalfwidthKatakana), "’”〜『』");
  EXPECT_EQ(BuildCommitForm("ぱーてぃー", CommitForm::HalfwidthKatakana), "ﾊﾟｰﾃｨｰ");
}

TEST(CommitFormTest, RomanFormsRespellInLowercaseHepburnWithoutTypedRomaji) {
  EXPECT_EQ(BuildCommitForm("だい3かい", CommitForm::HalfwidthRoman), "dai3kai");
  EXPECT_EQ(BuildCommitForm("だい3かい", CommitForm::FullwidthRoman), "ｄａｉ３ｋａｉ");
  EXPECT_EQ(BuildCommitForm("かっこ「", CommitForm::HalfwidthRoman), "kakko[");
  EXPECT_EQ(BuildCommitForm("かっこ「", CommitForm::FullwidthRoman), "ｋａｋｋｏ［");
  EXPECT_EQ(BuildCommitForm("すし", CommitForm::HalfwidthRoman), "sushi");
  EXPECT_EQ(BuildCommitForm("じ：かん", CommitForm::HalfwidthRoman), "ji:kan");
  EXPECT_EQ(BuildCommitForm("”＃＄％＆’（）＝〜｜｀『』＋＊＜＞＿＾＼￥＠；・？！",
                            CommitForm::HalfwidthRoman),
            "\"#$%&'()=~|`{}+*<>_^\\¥@;/?!");
  EXPECT_EQ(BuildCommitForm("￥", CommitForm::FullwidthRoman), "￥");
}

TEST(CommitFormTest, RespellsKanaRomanizeCannotSpellKanaByKana) {
  EXPECT_EQ(BuildCommitForm("ふぁ", CommitForm::HalfwidthRoman), "fuxa");
  EXPECT_EQ(BuildCommitForm("がっこうふぁん", CommitForm::HalfwidthRoman), "gaxtukoufuxan");
  EXPECT_EQ(BuildCommitForm("ふぁ3がっこう", CommitForm::HalfwidthRoman), "fuxa3gakkou");
  EXPECT_EQ(BuildCommitForm("あっ", CommitForm::HalfwidthRoman), "axtu");
  EXPECT_EQ(BuildCommitForm("ーあ", CommitForm::HalfwidthRoman), "-a");
}
