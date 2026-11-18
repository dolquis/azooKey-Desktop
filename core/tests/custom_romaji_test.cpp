#include <gtest/gtest.h>

#include <cctype>
#include <string>
#include <vector>

#include "azookey/core/CustomRomajiLoader.h"
#include "azookey/core/RomajiKanaConverter.h"

namespace {

using azookey::core::CustomRomajiLoader;
using azookey::core::RomajiKanaConverter;

std::string FeedAll(RomajiKanaConverter& converter, const std::string& input) {
  std::string output;
  for (char c : input) output += converter.Feed(c);
  return output + converter.Flush();
}

}  // namespace

TEST(CustomRomajiTest, ParsesBomCommentsCrLfDuplicatesAndConsume) {
  const auto result =
      CustomRomajiLoader::Parse("\xef\xbb\xbf# comment\r\n\r\nxa\tぁ\r\nxa\tァ\t1\r\n@@\t@\t2\r\n");
  EXPECT_TRUE(result.invalid_lines.empty());
  ASSERT_NE(result.table, nullptr);
  ASSERT_EQ(result.table->size(), 2u);
  EXPECT_EQ(result.table->at("xa").output, "ァ");
  EXPECT_EQ(result.table->at("xa").consume, 1u);
  EXPECT_EQ(result.table->at("@@").output, "@");
  EXPECT_EQ(result.table->at("@@").consume, 2u);
}

TEST(CustomRomajiTest, SkipsInvalidRowsAndReportsOneBasedLineNumbers) {
  const std::string invalid_utf8 = std::string("z\t") + static_cast<char>(0xff) + "\n";
  const auto result = CustomRomajiLoader::Parse(
      "good\tよい\n"
      "toolonggg\t長い\n"
      "é\tえ\n"
      "bad\t出力\t0\n"
      "bad\t出力\t4\n"
      "bad\t出力\tx\n"
      "bad\t出力\t2\tmore\n"
      "missing separator\n"
      "eight\t123456789\n" +
      invalid_utf8 + "good\t良い\n");
  EXPECT_EQ(result.invalid_lines, (std::vector<size_t>{2, 3, 4, 5, 6, 7, 8, 9, 10}));
  ASSERT_NE(result.table, nullptr);
  ASSERT_EQ(result.table->size(), 1u);
  EXPECT_EQ(result.table->at("good").output, "良い");
}

TEST(CustomRomajiTest, CountsSupplementaryCharactersAsTwoUtf16Units) {
  const auto result = CustomRomajiLoader::Parse("four\t😀😀😀😀\nfive\t😀😀😀😀😀\nempty\t\n");
  EXPECT_EQ(result.invalid_lines, (std::vector<size_t>{2}));
  EXPECT_EQ(result.table->at("four").output, "😀😀😀😀");
  EXPECT_EQ(result.table->at("empty").output, "");
}

TEST(CustomRomajiTest, EmptyOrInvalidOnlyTableFallsBackToBuiltIn) {
  for (const std::string tsv :
       {std::string{}, std::string{"# comment\n"}, std::string{"\xff\xfe"}}) {
    const auto result = CustomRomajiLoader::Parse(tsv);
    EXPECT_FALSE(result.table);
    EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("ka", result.table), "か");
  }
}

TEST(CustomRomajiTest, AcceptsEightAsciiInputAndRejectsInvalidUtf8Sequences) {
  const std::string invalid_utf8 = std::string("bad\t") + "\xed\xa0\x80" + "\n";
  const auto result = CustomRomajiLoader::Parse("12345678\t八\n" + invalid_utf8);
  EXPECT_EQ(result.invalid_lines, (std::vector<size_t>{2}));
  EXPECT_EQ(result.table->at("12345678").consume, 8u);
}

TEST(CustomRomajiTest, CustomTableReplacesBuiltInMappings) {
  const auto parsed = CustomRomajiLoader::Parse("ka\tカ\n@@\t@\n");
  RomajiKanaConverter converter;
  converter.SetCustomTable(parsed.table);
  EXPECT_EQ(FeedAll(converter, "ka"), "カ");
  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "KA"), "カ");
  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "sa"), "sa");
  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "@@"), "@");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("ka", parsed.table), "カ");
  EXPECT_EQ(RomajiKanaConverter::Preview("ka", parsed.table), "カ");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("ka"), "か");
}

// docs/legacy-parity-spec.md section 5.1 (M70): the rule keeps its letter case, so a rule
// written in capitals matches only Shift-typed letters.
TEST(CustomRomajiTest, KeepsTheCaseOfRuleKeys) {
  const auto result = CustomRomajiLoader::Parse("KA\t大\nka\t小\nKa\t混\n");
  ASSERT_TRUE(result.table);
  EXPECT_EQ(result.table->size(), 3u);
  EXPECT_EQ(result.table->at("KA").output, "大");
  EXPECT_EQ(result.table->at("ka").output, "小");
  EXPECT_EQ(result.table->at("Ka").output, "混");
}

// External behavior change (M70): a rule written in capitals no longer matches the unshifted
// keys. A version that folded the table to lowercase matched them whatever Shift said.
TEST(CustomRomajiTest, CapitalRuleMatchesOnlyShiftTypedLetters) {
  const auto table = CustomRomajiLoader::Parse("KA\tカ\n").table;
  RomajiKanaConverter converter;
  converter.SetCustomTable(table);
  EXPECT_EQ(FeedAll(converter, "KA"), "カ");
  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "ka"), "ka");
  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "Ka"), "ka");
  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "kA"), "ka");
}

TEST(CustomRomajiTest, ShiftTypedLettersFallBackToLowercaseRules) {
  const auto table = CustomRomajiLoader::Parse("ka\tか\nshi\tし\n").table;
  RomajiKanaConverter converter;
  converter.SetCustomTable(table);
  EXPECT_EQ(FeedAll(converter, "KA"), "か");
  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "Ka"), "か");
  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "SHi"), "し");
}

// A table with no capital rule reads Shift-typed letters as if they were lowercase.
TEST(CustomRomajiTest, TableWithoutCapitalRulesIgnoresTheCaseOfTheKeys) {
  const auto table = CustomRomajiLoader::Parse("ka\tか\nkb\tX\nz.\t…\nn\tん\t1\n").table;
  for (const char* lower : {"ka", "kb", "z.", "kak", "nk", "kz", "zz.", "q", "kakb"}) {
    std::string upper(lower);
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    RomajiKanaConverter lower_converter;
    lower_converter.SetCustomTable(table);
    RomajiKanaConverter upper_converter;
    upper_converter.SetCustomTable(table);
    EXPECT_EQ(FeedAll(upper_converter, upper), FeedAll(lower_converter, lower)) << lower;
  }
}

// A rule with the same case that is still a prefix wins over the lowercase rule of a shorter
// match, so the next key is awaited (section 5.1).
TEST(CustomRomajiTest, CapitalPrefixWaitsBeforeTheLowercaseRule) {
  const auto table = CustomRomajiLoader::Parse("KA\tX\nk\tY\n").table;
  RomajiKanaConverter converter;
  converter.SetCustomTable(table);
  EXPECT_EQ(converter.Feed('K'), "");
  EXPECT_TRUE(converter.HasPending());
  EXPECT_EQ(converter.PreviewPending(), "k");
  EXPECT_EQ(converter.Feed('A'), "X");
  EXPECT_FALSE(converter.HasPending());
  EXPECT_EQ(converter.Flush(), "");
}

// Awaited, but the next key does not continue the capital rule: the pending key is converted
// by the lowercase rule and the new key is looked up again.
TEST(CustomRomajiTest, FallsBackToTheLowercaseRuleWhenTheCapitalRuleDoesNotContinue) {
  const auto table = CustomRomajiLoader::Parse("KA\tX\nk\tY\nx\tZ\n").table;
  RomajiKanaConverter converter;
  converter.SetCustomTable(table);
  EXPECT_EQ(converter.Feed('K'), "");
  EXPECT_EQ(converter.Feed('x'), "YZ");
  EXPECT_EQ(converter.Flush(), "");
  converter.Reset();
  // The lowercase `a` is not the capital rule's `A`.
  EXPECT_EQ(converter.Feed('K'), "");
  EXPECT_EQ(converter.Feed('a'), "Ya");
  EXPECT_EQ(converter.Flush(), "");
  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "K"), "Y");
}

// When both a capital rule and the lowercase rule of the same letters exist, the rule with the
// same case as the keys wins; a key with no same-case rule uses the lowercase rule.
TEST(CustomRomajiTest, SameCaseRuleBeatsTheLowercaseFallback) {
  const auto table = CustomRomajiLoader::Parse("KA\t大\nka\t小\nKa\t混\n").table;
  RomajiKanaConverter converter;
  converter.SetCustomTable(table);
  EXPECT_EQ(FeedAll(converter, "KA"), "大");
  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "ka"), "小");
  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "Ka"), "混");
  converter.Reset();
  // `kA` has no same-case rule, so the lowercase rule is used.
  EXPECT_EQ(FeedAll(converter, "kA"), "小");
}

TEST(CustomRomajiTest, CanContinueFollowsTheCaseOfTheCapitalRule) {
  const auto table = CustomRomajiLoader::Parse("KA\tX\nsa\tY\n").table;
  RomajiKanaConverter converter;
  converter.SetCustomTable(table);
  EXPECT_TRUE(converter.CanContinueCustomWith('K'));
  EXPECT_FALSE(converter.CanContinueCustomWith('k'));
  // A Shift-typed letter reaches the lowercase rule.
  EXPECT_TRUE(converter.CanContinueCustomWith('S'));
  EXPECT_TRUE(converter.CanContinueCustomWith('s'));
  EXPECT_EQ(converter.Feed('K'), "");
  EXPECT_TRUE(converter.CanContinueCustomWith('A'));
  EXPECT_FALSE(converter.CanContinueCustomWith('a'));
}

// A letter that matches no rule stays in the reading in lowercase, pending or flushed.
TEST(CustomRomajiTest, UnmatchedLettersStayLowercase) {
  const auto table = CustomRomajiLoader::Parse("ab\tX\n").table;
  RomajiKanaConverter converter;
  converter.SetCustomTable(table);
  EXPECT_EQ(FeedAll(converter, "ZQ"), "zq");
  converter.Reset();
  EXPECT_EQ(converter.Feed('A'), "");
  EXPECT_EQ(converter.PreviewPending(), "a");
  EXPECT_EQ(converter.Flush(), "a");
  EXPECT_EQ(RomajiKanaConverter::Preview("AB", table), "X");
  EXPECT_EQ(RomajiKanaConverter::Preview("A", table), "a");
}

// The batch conversion keeps its raw input in lowercase, so it reads lowercase rules only.
TEST(CustomRomajiTest, BatchRawInputInLowercaseReadsLowercaseRulesOnly) {
  const auto table = CustomRomajiLoader::Parse("KA\tカ\nsa\tさ\n").table;
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("ka", table), "ka");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("sa", table), "さ");
}

TEST(CustomRomajiTest, CustomAsciiSymbolsAndDigitsAreRulePrefixes) {
  const auto table = CustomRomajiLoader::Parse("z.\t…\n@@\t@\n12\t十二\n").table;
  RomajiKanaConverter converter;
  converter.SetCustomTable(table);
  EXPECT_TRUE(converter.CanContinueCustomWith('z'));
  EXPECT_FALSE(converter.CanContinueCustomWith('.'));
  EXPECT_EQ(converter.Feed('z'), "");
  EXPECT_TRUE(converter.CanContinueCustomWith('.'));
  EXPECT_EQ(converter.Feed('.'), "…");
  EXPECT_EQ(converter.Flush(), "");
  converter.Reset();
  EXPECT_EQ(converter.Feed('z'), "");
  EXPECT_TRUE(converter.CanContinueCustomWith('@'));
  EXPECT_EQ(converter.Feed('@'), "z");
  EXPECT_EQ(FeedAll(converter, "@"), "@");
  converter.Reset();
  EXPECT_TRUE(converter.CanContinueCustomWith('@'));
  EXPECT_EQ(FeedAll(converter, "@@"), "@");
  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "12"), "十二");
}

TEST(CustomRomajiTest, ChoosesLongestMatchAndRetainsPartialPrefixUntilFlush) {
  const auto parsed = CustomRomajiLoader::Parse("a\tA\nab\tB\n");
  RomajiKanaConverter converter;
  converter.SetCustomTable(parsed.table);
  EXPECT_EQ(converter.Feed('a'), "");
  EXPECT_EQ(converter.PreviewPending(), "a");
  EXPECT_EQ(converter.Feed('b'), "B");
  EXPECT_EQ(converter.Flush(), "");
  EXPECT_EQ(converter.Feed('a'), "");
  EXPECT_EQ(converter.Flush(), "A");
  EXPECT_EQ(FeedAll(converter, "ax"), "Ax");
}

TEST(CustomRomajiTest, ConsumeReprocessesRemainingInput) {
  const auto parsed = CustomRomajiLoader::Parse("ab\tX\t1\nb\tY\n");
  RomajiKanaConverter converter;
  converter.SetCustomTable(parsed.table);
  EXPECT_EQ(FeedAll(converter, "ab"), "XY");
}

TEST(CustomRomajiTest, ExistingConverterKeepsSnapshotWhenNewTableLoads) {
  RomajiKanaConverter old_converter;
  old_converter.SetCustomTable(CustomRomajiLoader::Parse("ab\t旧\n").table);
  EXPECT_EQ(old_converter.Feed('a'), "");
  const auto new_table = CustomRomajiLoader::Parse("ab\t新\n").table;
  RomajiKanaConverter new_converter;
  new_converter.SetCustomTable(new_table);
  EXPECT_EQ(old_converter.Feed('b'), "旧");
  EXPECT_EQ(FeedAll(new_converter, "ab"), "新");
  new_converter.SetCustomTable(nullptr);
  new_converter.Reset();
  EXPECT_EQ(FeedAll(new_converter, "ka"), "か");
}
