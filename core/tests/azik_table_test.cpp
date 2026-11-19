#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "azookey/core/AzikTable.h"
#include "azookey/core/RomajiKanaConverter.h"

namespace {

using azookey::core::BuiltinAzikTable;
using azookey::core::RomajiKanaConverter;

std::string AzikReading(const std::string& keys) {
  RomajiKanaConverter converter;
  converter.SetCustomTable(BuiltinAzikTable());
  std::string output;
  for (const char key : keys) output += converter.Feed(key);
  return output + converter.Flush();
}

}  // namespace

// docs/input-style-spec.md section 8: upstream's 569 rules plus "-" and "ji".
TEST(AzikTableTest, HasUpstreamRulesPlusTheTwoWindowsAdditions) {
  const auto table = BuiltinAzikTable();
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->size(), 571u);
  EXPECT_EQ(BuiltinAzikTable(), table);
  for (const auto& [key, rule] : *table) {
    EXPECT_GE(key.size(), 1u) << key;
    EXPECT_LE(key.size(), 3u) << key;
    EXPECT_EQ(rule.consume, key.size()) << key;
    EXPECT_FALSE(rule.output.empty()) << key;
    for (const unsigned char byte : key) {
      EXPECT_LE(byte, 0x7fu) << "non-ASCII key: " << key;
      EXPECT_FALSE(byte >= 'A' && byte <= 'Z') << "uppercase key: " << key;
    }
  }
}

// Section 3.1: the seven full-width keys of upstream come back to the ASCII key that was typed.
TEST(AzikTableTest, RestoresTheFullWidthKeysToTheirAsciiKeys) {
  const auto table = BuiltinAzikTable();
  const std::pair<const char*, const char*> expected[] = {
      {";", "っ"}, {":", "ー"}, {"z.", "…"}, {"z,", "‥"}, {"z-", "〜"}, {"z[", "『"}, {"z]", "』"},
  };
  for (const auto& [key, output] : expected) {
    const auto rule = table->find(key);
    ASSERT_NE(rule, table->end()) << key;
    EXPECT_EQ(rule->second.output, output) << key;
  }
  EXPECT_EQ(table->count("；"), 0u);
  EXPECT_EQ(table->count("："), 0u);
  EXPECT_EQ(table->count("z。"), 0u);
  EXPECT_EQ(table->count("z、"), 0u);
  EXPECT_EQ(table->count("zー"), 0u);
  EXPECT_EQ(table->count("z「"), 0u);
  EXPECT_EQ(table->count("z」"), 0u);
}

// A spot check of rules copied from upstream, spread over the table (the first, the last and
// the two-letter word rules), so a mis-copied output is not hidden by the matching rule count.
TEST(AzikTableTest, KeepsSampleRulesOfTheUpstreamTable) {
  const std::pair<const char*, const char*> expected[] = {
      {"a", "あ"},    {"ka", "か"},   {"dm", "でも"}, {"nr", "なる"}, {"mt", "また"},
      {"gr", "がら"}, {"wr", "われ"}, {"ht", "ひと"}, {"ds", "です"}, {"kr", "から"},
      {"yr", "よる"}, {"tb", "たび"}, {"gt", "ごと"},
  };
  const auto table = BuiltinAzikTable();
  for (const auto& [key, output] : expected) {
    const auto rule = table->find(key);
    ASSERT_NE(rule, table->end()) << key;
    EXPECT_EQ(rule->second.output, output) << key;
  }
}

// Sections 3.1 and 3.2: "-" and "ji" are added; upstream has no rule for a bare "n".
TEST(AzikTableTest, AddsHyphenAndJiButNoBareN) {
  const auto table = BuiltinAzikTable();
  ASSERT_EQ(table->count("-"), 1u);
  EXPECT_EQ(table->at("-").output, "ー");
  ASSERT_EQ(table->count("ji"), 1u);
  EXPECT_EQ(table->at("ji").output, "じ");
  EXPECT_EQ(table->count("n"), 0u);
}

// Section 3.3: the table-driven path skips Feed's built-in special cases, which AZIK rules
// collide with.
TEST(AzikTableTest, ConvertsThroughTheTableAndSkipsTheBuiltInSpecialCases) {
  EXPECT_EQ(AzikReading("kk"), "きん");
  EXPECT_EQ(AzikReading("nt"), "にち");
  EXPECT_EQ(AzikReading("nz"), "なん");
  EXPECT_EQ(AzikReading("nn"), "ん");
  EXPECT_EQ(AzikReading("ka"), "か");
  EXPECT_EQ(AzikReading("ji"), "じ");
  // The built-in path reads "kk" as a doubled consonant (っ + k) and "nt" as ん + t.
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("kk"), "っk");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("nt"), "んt");
}

TEST(AzikTableTest, ConvertsSymbolKeysByTheirAsciiKey) {
  EXPECT_EQ(AzikReading("-"), "ー");
  EXPECT_EQ(AzikReading("z-"), "〜");
  EXPECT_EQ(AzikReading(";"), "っ");
  EXPECT_EQ(AzikReading(":"), "ー");
  EXPECT_EQ(AzikReading("z."), "…");
  EXPECT_EQ(AzikReading("z,"), "‥");
  EXPECT_EQ(AzikReading("z["), "『");
  EXPECT_EQ(AzikReading("z]"), "』");
  EXPECT_EQ(AzikReading("ka-"), "かー");
}

// Section 3.2: a leftover "n" is not turned into ん at commit; it stays a letter.
TEST(AzikTableTest, LeavesAPendingNAsALetterAtCommit) {
  EXPECT_EQ(AzikReading("n"), "n");
  EXPECT_EQ(AzikReading("kan"), "かn");
  RomajiKanaConverter converter;
  converter.SetCustomTable(BuiltinAzikTable());
  EXPECT_EQ(converter.Feed('n'), "");
  EXPECT_EQ(converter.PreviewPending(), "n");
}

// Section 3.3: the table has no capital rules, so Shift-typed letters use the lowercase rules.
TEST(AzikTableTest, ShiftTypedLettersUseTheLowercaseRules) {
  EXPECT_EQ(AzikReading("KA"), "か");
  EXPECT_EQ(AzikReading("Kk"), "きん");
  EXPECT_EQ(AzikReading("ZJ"), "ずん");
  EXPECT_EQ(AzikReading("zj"), "ずん");
}

// Section 3.3: only `z` continues into . , [ ] -- a bare symbol key is not a rule prefix.
TEST(AzikTableTest, ContinuesOnlyAfterZForTheZSymbols) {
  RomajiKanaConverter converter;
  converter.SetCustomTable(BuiltinAzikTable());
  EXPECT_TRUE(converter.CanContinueCustomWith(';'));
  EXPECT_TRUE(converter.CanContinueCustomWith(':'));
  EXPECT_TRUE(converter.CanContinueCustomWith('-'));
  EXPECT_FALSE(converter.CanContinueCustomWith('.'));
  EXPECT_FALSE(converter.CanContinueCustomWith('['));
  EXPECT_EQ(converter.Feed('z'), "");
  EXPECT_TRUE(converter.CanContinueCustomWith('.'));
  EXPECT_TRUE(converter.CanContinueCustomWith(','));
  EXPECT_TRUE(converter.CanContinueCustomWith('['));
  EXPECT_TRUE(converter.CanContinueCustomWith(']'));
}
