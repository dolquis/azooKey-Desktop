#include <string>

#include <gtest/gtest.h>

#include "azookey/core/RomajiKanaConverter.h"

namespace {

std::string FeedAll(azookey::core::RomajiKanaConverter& converter,
                    const std::string& input) {
  std::string out;
  for (char c : input) {
    out += converter.Feed(c);
  }
  out += converter.Flush();
  return out;
}

}  // namespace

TEST(RomajiKanaConverterTest, FeedAndFlush) {
  azookey::core::RomajiKanaConverter converter;
  EXPECT_EQ(FeedAll(converter, "konnichiha"), "こんにちは");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "nani"), "なに");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "gakkou"), "がっこう");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "konn"), "こん");

  // 長音: '-' は長音符「ー」に変換される。
  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "ra-men"), "らーめん");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "ko-hi-"), "こーひー");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "janai"), "じゃない");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "juku"), "じゅく");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "jyou"), "じょう");
}

TEST(RomajiKanaConverterTest, AlternativeRomajiAliases) {
  azookey::core::RomajiKanaConverter converter;
  EXPECT_EQ(FeedAll(converter, "siro"), "しろ");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "tu"), "つ");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "syatu"), "しゃつ");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "tyotto"), "ちょっと");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "zibun"), "じぶん");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "di"), "ぢ");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "du"), "づ");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "zya"), "じゃ");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "dya"), "ぢゃ");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "huji"), "ふじ");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "ttuki"), "っつき");
}

TEST(RomajiKanaConverterTest, Preview) {
  using azookey::core::RomajiKanaConverter;
  EXPECT_EQ(RomajiKanaConverter::Preview("k"), "k");
  EXPECT_EQ(RomajiKanaConverter::Preview("ka"), "か");
  EXPECT_EQ(RomajiKanaConverter::Preview("n"), "ん");
  EXPECT_EQ(RomajiKanaConverter::Preview("kan"), "かん");
  EXPECT_EQ(RomajiKanaConverter::Preview("na"), "な");
  EXPECT_EQ(RomajiKanaConverter::Preview("ny"), "ny");
  EXPECT_EQ(RomajiKanaConverter::Preview("konn"), "こん");
  EXPECT_EQ(RomajiKanaConverter::Preview("konnichiha"), "こんにちは");
  EXPECT_EQ(RomajiKanaConverter::Preview("gakkou"), "がっこう");
  EXPECT_EQ(RomajiKanaConverter::Preview("ko-"), "こー");
  EXPECT_EQ(RomajiKanaConverter::Preview("ja"), "じゃ");
  EXPECT_EQ(RomajiKanaConverter::Preview("ju"), "じゅ");
  EXPECT_EQ(RomajiKanaConverter::Preview("jo"), "じょ");
  EXPECT_EQ(RomajiKanaConverter::Preview("je"), "じぇ");
  EXPECT_EQ(RomajiKanaConverter::Preview("jya"), "じゃ");
  EXPECT_EQ(RomajiKanaConverter::Preview("jyu"), "じゅ");
  EXPECT_EQ(RomajiKanaConverter::Preview("jyo"), "じょ");
  EXPECT_EQ(RomajiKanaConverter::Preview("si"), "し");
  EXPECT_EQ(RomajiKanaConverter::Preview("tu"), "つ");
  EXPECT_EQ(RomajiKanaConverter::Preview("sya"), "しゃ");
}

TEST(RomajiKanaConverterTest, ConvertForCommit) {
  using azookey::core::RomajiKanaConverter;
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("kan"), "かん");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("na"), "な");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("konn"), "こん");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("me-ru"), "めーる");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("janken"), "じゃんけん");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("juu"), "じゅう");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("jojo"), "じょじょ");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("jenga"), "じぇんが");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("siro"), "しろ");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("syatu"), "しゃつ");
}
