#include <gtest/gtest.h>

#include "azookey/core/RewriterIndex.h"

namespace azookey::core {
namespace {
TEST(RewriterNormalization, NormalizesCompatibilityKanaAndKeepsLongVowels) {
  EXPECT_EQ(NormalizeRewriterReading(" ﾗｰﾒﾝ　"), "らーめん");
  EXPECT_EQ(NormalizeRewriterReading("ｶﾞｯﾂ\tポーズ"), "がっつぽーず");
  EXPECT_EQ(NormalizeRewriterReading("ガ"), "が");
  EXPECT_EQ(NormalizeRewriterReading("㌀"), "あぱーと");
  EXPECT_EQ(NormalizeRewriterReading("らーめん"), "らーめん");
  EXPECT_EQ(NormalizeRewriterReading("😄"), "😄");
  EXPECT_EQ(NormalizeRewriterReading("\xff"), "");
  EXPECT_EQ(NormalizeEmojiTrigger(":SmILe_+1-!"), "smile_+1-");
}

TEST(RewriterIndex, ReadingIsExactRankedAndCappedAtFour) {
  RewriterIndex index(CandidateSource::Symbol);
  ASSERT_EQ(index.Parse("a\tかっこ\tA\t1\n"
                        "b\tかっこ\tB\t2\n"
                        "c\tかっこ\tC\t2\n"
                        "d\tかっこ\tD\t4\n"
                        "e\tかっこ\tE\t3\n"),
            0u);
  const auto result = index.LookupReading("ｶｯｺ");
  ASSERT_EQ(result.size(), 4u);
  EXPECT_EQ(result[0].surface, "d");
  EXPECT_EQ(result[1].surface, "e");
  EXPECT_EQ(result[2].surface, "b");
  EXPECT_EQ(result[3].surface, "c");
  EXPECT_EQ(result[0].reading, "ｶｯｺ");
  EXPECT_EQ(result[0].description, "D");
  EXPECT_EQ(result[0].source, CandidateSource::Symbol);
  EXPECT_TRUE(index.LookupReading("かっ").empty());
  EXPECT_TRUE(index.LookupReading(std::string(33, 'a')).empty());
  EXPECT_TRUE(index.SearchTrigger("a", 12).empty());
}

TEST(RewriterIndex, AcceptsWindowsBomAndCrlfWithoutChangingSurface) {
  RewriterIndex index(CandidateSource::Symbol);
  ASSERT_EQ(index.Parse("\xef\xbb\xbf"
                        "☀\tたいよう\t太陽\t1\r\n"),
            0u);
  ASSERT_EQ(index.LookupReading("たいよう").size(), 1u);
  EXPECT_EQ(index.LookupReading("たいよう")[0].surface, "☀");
  EXPECT_EQ(index.Parse("\xef\xbb\xbf"
                        "# comment\r\n☀\tたいよう\t太陽\t1\r\n"),
            0u);
}

TEST(RewriterIndex, TriggerRankingChoosesBestAliasBeforeTruncation) {
  RewriterIndex index(CandidateSource::Emoji);
  ASSERT_EQ(index.Parse("😄\tわらい\tsmile|smiley\t笑顔\t1\n"
                        "😊\tえがお\tsmiles\t笑み\t99\n"
                        "☺️\t\tsmall\tにこにこ\t100\n"
                        "😁\t\tasmile\t歯を見せた笑顔\t1000\n"),
            0u);
  auto result = index.SearchTrigger("smile", 12);
  ASSERT_EQ(result.size(), 3u);
  EXPECT_EQ(result[0].surface, "😄");
  EXPECT_EQ(result[1].surface, "😊");
  result = index.SearchTrigger("smi", 12);
  ASSERT_EQ(result.size(), 3u);
  EXPECT_EQ(result[0].surface, "😄");
  EXPECT_EQ(result[1].surface, "😊");
  EXPECT_EQ(result[2].surface, "😁");
  result = index.SearchTrigger("smle", 1);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].surface, "😄");
  EXPECT_TRUE(result[0].reading.empty());
  EXPECT_EQ(result[0].source, CandidateSource::Emoji);
  EXPECT_TRUE(index.SearchTrigger("smlie", 12).empty());
  EXPECT_TRUE(index.SearchTrigger("m", 12).empty());
  EXPECT_TRUE(index.SearchTrigger("!", 12).empty());
  EXPECT_TRUE(index.SearchTrigger(std::string(33, 's'), 12).empty());
  EXPECT_EQ(index.LookupReading("ワライ")[0].surface, "😄");
}

TEST(RewriterIndex, SkipsMalformedRowsAndPreservesSequences) {
  RewriterIndex index(CandidateSource::Emoji);
  EXPECT_EQ(index.Parse("# attribution\n"
                        "👩‍💻\tしごと\twoman_technologist\t技術者\t1\n"
                        "☺️\tわらい\tsmile\t笑顔\t2\n"
                        "a\t\t\tempty keys\t1\n"
                        "b\tかな||よみ\tb\tbad\t1\n"
                        "c\tかな\tc|c\tbad\t1\n"
                        "d\tかな\td\tbad|name\t1\n"
                        "e\tかな\te\tbad\t-1\n"
                        "f\tかな\tf\tbad\t4294967296\n"
                        "g\tカナ\tg\tbad\t1\n"
                        "h\tかな\tH\tbad\t1\n"
                        "👩‍💻\tかな\tduplicate\tbad\t1\n"
                        "i\tかな\ti\tbad\t1\textra\n"),
            10u);
  ASSERT_EQ(index.size(), 2u);
  EXPECT_EQ(index.LookupReading("しごと")[0].surface, "👩‍💻");
  EXPECT_EQ(index.SearchTrigger("smile", 12)[0].surface, "☺️");
  EXPECT_GT(index.EstimatedMemoryBytes(), 0u);
  EXPECT_EQ(index.Parse(""), 0u);
  EXPECT_EQ(index.size(), 0u);
  EXPECT_TRUE(index.LookupReading("しごと").empty());
}
}  // namespace
}  // namespace azookey::core
