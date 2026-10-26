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

TEST(RomajiKanaConverterTest, LegacyDefaultTableSpellings) {
  azookey::core::RomajiKanaConverter converter;
  EXPECT_EQ(FeedAll(converter, "xtulawive"), "っぁうぃゔぇ");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "ltsu"), "っ");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "xtsuxya"), "っゃ");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "fakkusu"), "ふぁっくす");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "thi"), "てぃ");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "yewyixn"), "いぇゐん");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "zhzl"), "←→");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "kyashuchann"), "きゃしゅちゃん");

  // The legacy table has no "tch" rule, so the leading t stays literal.
  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "tcha"), "tちゃ");
}

TEST(RomajiKanaConverterTest, PendingLegacyPrefixIsHeldUntilResolved) {
  azookey::core::RomajiKanaConverter converter;
  EXPECT_EQ(converter.Feed('x'), "");
  EXPECT_EQ(converter.Feed('t'), "");
  EXPECT_EQ(converter.Feed('s'), "");
  EXPECT_EQ(converter.PreviewPending(), "xts");
  EXPECT_EQ(converter.Feed('u'), "っ");
  EXPECT_FALSE(converter.HasPending());

  // A three-letter pending run that no key continues still releases its head.
  EXPECT_EQ(converter.Feed('k'), "");
  EXPECT_EQ(converter.Feed('t'), "");
  EXPECT_EQ(converter.Feed('a'), "kた");
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

TEST(RomajiKanaConverterTest, PopPendingPreviewRemovesDisplayedPreviewUnit) {
  azookey::core::RomajiKanaConverter converter;
  EXPECT_EQ(converter.Feed('n'), "");
  EXPECT_EQ(converter.Feed('n'), "");
  EXPECT_EQ(converter.PreviewPending(), "ん");

  converter.PopPendingPreview();
  EXPECT_FALSE(converter.HasPending());
  EXPECT_EQ(converter.PreviewPending(), "");

  EXPECT_EQ(converter.Feed('n'), "");
  EXPECT_EQ(converter.Feed('y'), "");
  EXPECT_EQ(converter.PreviewPending(), "ny");

  converter.PopPendingPreview();
  EXPECT_TRUE(converter.HasPending());
  EXPECT_EQ(converter.PreviewPending(), "ん");
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

// The built-in table carries the second n of "nn" + vowel/y into the next syllable.
TEST(RomajiKanaConverterTest, DoubleNBeforeVowelCarriesN) {
  using azookey::core::RomajiKanaConverter;
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("genninn"), "げんにん");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("naibugenninn"), "ないぶげんにん");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("konnichiha"), "こんにちは");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("kanni"), "かんに");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("sannin"), "さんにん");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("tannin"), "たんにん");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("onna"), "おんな");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("minna"), "みんな");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("kinnyuu"), "きんにゅう");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("konnnichiha"), "こんにちは");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("gennninn"), "げんにん");
}

TEST(RomajiKanaConverterTest, AmbiguousReadingsListBothDoubleNReadings) {
  using azookey::core::RomajiKanaConverter;
  using Readings = std::vector<std::string>;
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("genninn"), (Readings{"げんにん", "げんいん"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("naibugenninn"),
            (Readings{"ないぶげんにん", "ないぶげんいん"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("konnichiha"),
            (Readings{"こんにちは", "こんいちは"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("kanni"), (Readings{"かんに", "かんい"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("sannin"), (Readings{"さんにん", "さんいん"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("tannin"), (Readings{"たんにん", "たんいん"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("onna"), (Readings{"おんな", "おんあ"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("minna"), (Readings{"みんな", "みんあ"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("kinnyuu"),
            (Readings{"きんにゅう", "きんゆう"}));
}

TEST(RomajiKanaConverterTest, AmbiguousReadingsSkipsExplicitSpellings) {
  using azookey::core::RomajiKanaConverter;
  using Readings = std::vector<std::string>;
  // "nnn" + vowel spells ん + な行 explicitly; a trailing "nn" has no following syllable.
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("konnnichiha"), (Readings{"こんにちは"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("gennninn"), (Readings{"げんにん"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("konn"), (Readings{"こん"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("nihongo"), (Readings{"にほんご"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings(""), (Readings{""}));
  // Reading "nnyi" as ん + "yi" leaves romaji the default reading does not have.
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("kinnyi"), (Readings{"きんにぃ"}));
  // A trailing "nny" is still being typed; ん + "y" would drop the pending n instead.
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("konny"), (Readings{"こんny"}));
  // Only the position that converts cleanly varies.
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("onnakinnyi"),
            (Readings{"おんなきんにぃ", "おんあきんにぃ"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("onnakonny"),
            (Readings{"おんなこんny", "おんあこんny"}));
}

TEST(RomajiKanaConverterTest, AmbiguousReadingsFollowFeedInputHandling) {
  using azookey::core::RomajiKanaConverter;
  using Readings = std::vector<std::string>;
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("KONNICHIHA"),
            (Readings{"こんにちは", "こんいちは"}));
  // "nn" before a symbol or digit is ん with no following syllable to reinterpret.
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("konn1"), (Readings{"こん1"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("onn-"), (Readings{"おんー"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("onn!a"), (Readings{"おん!あ"}));
}

TEST(RomajiKanaConverterTest, AmbiguousReadingsCapsAmbiguousPositions) {
  using azookey::core::RomajiKanaConverter;
  using Readings = std::vector<std::string>;
  const Readings four = RomajiKanaConverter::AmbiguousReadings("onnaonnaonnaonna");
  ASSERT_EQ(four.size(), 8u);
  EXPECT_EQ(four.front(), "おんなおんなおんなおんな");
  EXPECT_EQ(four.back(), "おんあおんあおんあおんな");
  for (const std::string& reading : four) {
    EXPECT_TRUE(reading.ends_with("おんな")) << reading;
  }

  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("onnaminna", 1),
            (Readings{"おんなみんな", "おんあみんな"}));
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("onna", 0), (Readings{"おんな"}));
  // A larger limit is still capped at kMaxAmbiguousDoubleN positions.
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("onnaonnaonnaonna", 5), four);
}

TEST(RomajiKanaConverterTest, AmbiguousReadingsUseOnlyTheCustomTable) {
  using azookey::core::RomajiKanaConverter;
  const auto parsed = azookey::core::CustomRomajiLoader::Parse("nn\tん\t2\nna\tな\na\tあ\n");
  ASSERT_TRUE(parsed.table);
  EXPECT_EQ(RomajiKanaConverter::AmbiguousReadings("nna", parsed.table),
            (std::vector<std::string>{"んあ"}));
}
