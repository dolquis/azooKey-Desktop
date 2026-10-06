#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

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

// Fixed index for the spec 9.3/9.4 expectation table. Every row is a distinct
// emoji so the ordering key (class, key length, rank, key, emoji) is observable.
const char kOrderingTable[] =
    "😄\t\tsmile|smiley\t笑顔\t10\n"
    "😆\t\tsmile\t大笑い\t10\n"
    "🙂\t\tsmile\t微笑み\t20\n"
    "😊\t\tsmiles\t笑み\t99\n"
    "😺\t\tsmilecat\t猫の笑顔\t500\n"
    "😁\t\tasmile\t歯を見せた笑顔\t1000\n"
    "🤣\t\tsmxile\t爆笑\t5\n"
    "🐍\t\tsnake\t蛇\t1\n";

std::vector<std::string> Surfaces(const std::vector<Candidate>& candidates) {
  std::vector<std::string> result;
  for (const auto& candidate : candidates) result.push_back(candidate.surface);
  return result;
}

TEST(RewriterIndexOrdering, ClassThenKeyLengthThenRankThenKeyThenEmoji) {
  RewriterIndex index(CandidateSource::Emoji);
  ASSERT_EQ(index.Parse(kOrderingTable), 0u);
  // Exact: rank desc, then emoji code points on a full tie (😄 U+1F604 < 😆 U+1F606).
  // The shorter Prefix key (smiles) precedes the longer one (smilecat) despite its lower rank,
  // and every Subsequence match follows every Prefix match whatever its rank.
  EXPECT_EQ(Surfaces(index.SearchTrigger("smile", 0)),
            (std::vector<std::string>{"🙂", "😄", "😆", "😊", "😺", "😁", "🤣"}));
  // 😄 matches smile (Exact) and smiley (Prefix); only the best alias represents it.
  const auto smi = Surfaces(index.SearchTrigger("smi", 0));
  EXPECT_EQ(std::count(smi.begin(), smi.end(), "😄"), 1);
  EXPECT_EQ(smi, (std::vector<std::string>{"🙂", "😄", "😆", "😊", "😺", "😁", "🤣"}));
}

TEST(RewriterIndexOrdering, LimitIsAppliedAfterSortingAndZeroMeansUnlimited) {
  RewriterIndex index(CandidateSource::Emoji);
  ASSERT_EQ(index.Parse(kOrderingTable), 0u);
  EXPECT_EQ(Surfaces(index.SearchTrigger("smile", 1)), (std::vector<std::string>{"🙂"}));
  EXPECT_EQ(Surfaces(index.SearchTrigger("smile", 4)),
            (std::vector<std::string>{"🙂", "😄", "😆", "😊"}));
  EXPECT_EQ(index.SearchTrigger("smile", 50).size(), 7u);
  EXPECT_EQ(index.SearchTrigger("smile", 0).size(), 7u);
  EXPECT_EQ(Surfaces(index.SearchTrigger("smile", 7)), Surfaces(index.SearchTrigger("smile", 0)));
}

TEST(RewriterIndexOrdering, SingleCharacterQueryEvaluatesOnlyExactAndPrefix) {
  RewriterIndex index(CandidateSource::Emoji);
  ASSERT_EQ(index.Parse("🅢\t\ts\t文字S\t1\n"
                        "🐍\t\tsnake\t蛇\t1\n"
                        "🧱\t\tbass\t低音\t1\n"),
            0u);
  // bass contains s as a subsequence but is excluded for a one-character query.
  EXPECT_EQ(Surfaces(index.SearchTrigger("s", 12)), (std::vector<std::string>{"🅢", "🐍"}));
  EXPECT_EQ(Surfaces(index.SearchTrigger("S", 12)), (std::vector<std::string>{"🅢", "🐍"}));
  // Two characters re-enable Subsequence: sk matches snake, ss matches bass.
  EXPECT_EQ(Surfaces(index.SearchTrigger("sk", 12)), (std::vector<std::string>{"🐍"}));
  EXPECT_EQ(Surfaces(index.SearchTrigger("ss", 12)), (std::vector<std::string>{"🧱"}));
}

TEST(RewriterIndexOrdering, TriggerQueryIsNormalizedAndPlusSurvives) {
  RewriterIndex index(CandidateSource::Emoji);
  ASSERT_EQ(index.Parse("👍\t\t+1|thumbs_up\t親指\t1\n"), 0u);
  EXPECT_EQ(index.SearchTrigger("+1", 12).size(), 1u);
  EXPECT_EQ(index.SearchTrigger("+", 12).size(), 1u);
  EXPECT_EQ(index.SearchTrigger(":+1:", 12).size(), 1u);
  EXPECT_EQ(index.SearchTrigger("Thumbs_UP", 12).size(), 1u);
  EXPECT_TRUE(index.SearchTrigger("1", 12).empty());
  EXPECT_TRUE(index.SearchTrigger("::", 12).empty());
  EXPECT_TRUE(index.SearchTrigger("", 12).empty());
}

TEST(RewriterIndexOrdering, QueryLengthBoundaryIsThirtyTwo) {
  const std::string key32(32, 'a');
  RewriterIndex index(CandidateSource::Emoji);
  ASSERT_EQ(index.Parse("🅰\t\t" + key32 + "\t長い\t1\n"), 0u);
  EXPECT_EQ(index.SearchTrigger(key32, 12).size(), 1u);
  EXPECT_TRUE(index.SearchTrigger(key32 + "a", 12).empty());
  EXPECT_EQ(index.SearchTrigger(key32.substr(0, 31), 12).size(), 1u);
}

TEST(RewriterIndexOrdering, ReadingBoundaryAndKanaFoldingKeepLongVowel) {
  std::string reading32;
  for (int i = 0; i < 32; ++i) reading32 += "あ";
  RewriterIndex index(CandidateSource::Symbol);
  ASSERT_EQ(index.Parse("☆\t" + reading32 + "\t星\t1\n★\tらーめん\t黒星\t1\n"), 0u);
  EXPECT_EQ(index.LookupReading(reading32).size(), 1u);
  EXPECT_TRUE(index.LookupReading(reading32 + "あ").empty());
  EXPECT_EQ(index.LookupReading("ラーメン").size(), 1u);
  EXPECT_EQ(index.LookupReading("ﾗｰﾒﾝ").size(), 1u);
  EXPECT_TRUE(index.LookupReading("らめん").empty());
  EXPECT_TRUE(index.LookupReading("").empty());
  EXPECT_TRUE(index.LookupReading("ら").empty());
}
}  // namespace
}  // namespace azookey::core
