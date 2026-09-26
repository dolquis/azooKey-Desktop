#include <gtest/gtest.h>

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

TEST(CustomRomajiTest, AcceptsEightAsciiInputAndRejectsInvalidUtf8Sequences) {
  const std::string invalid_utf8 = std::string("bad\t") + "\xed\xa0\x80" + "\n";
  const auto result = CustomRomajiLoader::Parse("12345678\t八\n" + invalid_utf8);
  EXPECT_EQ(result.invalid_lines, (std::vector<size_t>{2}));
  EXPECT_EQ(result.table->at("12345678").consume, 8u);
}

TEST(CustomRomajiTest, CustomTableReplacesBuiltInMappings) {
  const auto parsed = CustomRomajiLoader::Parse("KA\tカ\n@@\t@\n");
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
