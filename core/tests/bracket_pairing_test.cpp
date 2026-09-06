#include <gtest/gtest.h>

#include "azookey/core/BracketPairing.h"
#include "azookey/core/BracketSettings.h"

namespace {
using namespace azookey::core;

BracketPairingOptions Enabled() {
  BracketPairingOptions options;
  options.enabled = true;
  return options;
}

TEST(BracketTableTest, MergesOverridesAdditionsAndDisablesByOpeningCharacter) {
  const auto result = ParseBracketTable("\xef\xbb\xbf# pairs\r\n(\t}\r\n<\t>\n[\t]\toff\n");
  EXPECT_TRUE(result.invalid_lines.empty());
  EXPECT_EQ(LookupBracketPair(U'(', result.table)->close, U'}');
  EXPECT_EQ(LookupBracketPair(U'<', result.table)->close, U'>');
  EXPECT_FALSE(LookupBracketPair(U'[', result.table));
  EXPECT_FALSE(LookupClosingBracket(U')', result.table));
  EXPECT_TRUE(LookupBracketPair(U'「', result.table));
  EXPECT_EQ(EvaluateBracketInput(U'<', false, {{}, {}, true}, Enabled(), result.table).close, U'>');
  EXPECT_EQ(EvaluateBracketBackspace(false, {U'(', U'}', true}, Enabled(), result.table).type,
            BracketPairingActionType::kDeletePair);
}

TEST(BracketTableTest, SkipsInvalidRowsWithoutDiscardingValidOverrides) {
  const auto result = ParseBracketTable(
      "bad\nxx\t)\n(\t\xff\n(\t)\tunknown\n"
      "(\t)\t\textra\n😀\t)\n \t)\n(\t}\n(\t]\n");
  EXPECT_EQ(result.invalid_lines, (std::vector<size_t>{1, 2, 3, 4, 5, 6, 7}));
  EXPECT_EQ(LookupBracketPair(U'(', result.table)->close, U']');
  EXPECT_EQ(ParseBracketTable("").table.pairs, BuiltinBracketTable().pairs);
}

TEST(BracketTableTest, SymmetricDefinitionsRemainDisabledWithoutOptIn) {
  const auto result = ParseBracketTable("\"\t\"\n");
  ASSERT_TRUE(LookupBracketPair(U'"', result.table));
  EXPECT_EQ(EvaluateBracketInput(U'"', false, {{}, {}, true}, Enabled(), result.table).type,
            BracketPairingActionType::kPassThrough);
  EXPECT_EQ(EvaluateBracketBackspace(false, {U'"', U'"', true}, Enabled(), result.table).type,
            BracketPairingActionType::kPassThrough);
}

TEST(BracketPairingTest, InsertsEveryBuiltInPairAtCollapsedCaret) {
  const std::u32string opens = U"（「『【〔［｛〈《“‘([{", closes = U"）」』】〕］｝〉》”’)]}";
  ASSERT_EQ(opens.size(), closes.size());
  for (size_t i = 0; i < opens.size(); ++i) {
    const auto action = EvaluateBracketInput(opens[i], false, {{}, {}, true}, Enabled());
    EXPECT_EQ(action.type, BracketPairingActionType::kInsertPair);
    EXPECT_EQ(action.open, opens[i]);
    EXPECT_EQ(action.close, closes[i]);
  }
}

TEST(BracketPairingTest, DisabledDoesNotInterceptAnyBracketOrBackspace) {
  for (char32_t cp : U"（）」』[]{}()\"'`<>") {
    EXPECT_EQ(EvaluateBracketInput(cp, false, {U'[', U']', true}, {}).type,
              BracketPairingActionType::kPassThrough);
  }
  EXPECT_EQ(EvaluateBracketBackspace(false, {U'[', U']', true}, {}).type,
            BracketPairingActionType::kPassThrough);
}

TEST(BracketPairingTest, UnknownOrNonCollapsedSelectionInsertsOnlyLiteral) {
  for (const auto collapsed : {std::optional<bool>{}, std::optional<bool>{false}}) {
    EXPECT_EQ(EvaluateBracketInput(U'「', false, {{}, {}, collapsed}, Enabled()).type,
              BracketPairingActionType::kInsertLiteral);
    EXPECT_EQ(EvaluateBracketInput(U'」', false, {U'「', U'」', collapsed}, Enabled()).type,
              BracketPairingActionType::kInsertLiteral);
    EXPECT_EQ(EvaluateBracketBackspace(false, {U'「', U'」', collapsed}, Enabled()).type,
              BracketPairingActionType::kPassThrough);
  }
}

TEST(BracketPairingTest, ClosingSkipsOnlyMatchingAdjacentCharacterWhenEnabled) {
  auto options = Enabled();
  EXPECT_EQ(EvaluateBracketInput(U'」', false, {U'あ', U'」', true}, options).type,
            BracketPairingActionType::kSkipClosing);
  for (auto after : {std::optional<char32_t>{}, std::optional<char32_t>{U'あ'},
                     std::optional<char32_t>{U')'}, std::optional<char32_t>{0xd800}}) {
    EXPECT_EQ(EvaluateBracketInput(U'」', false, {{}, after, true}, options).type,
              BracketPairingActionType::kInsertLiteral);
  }
  options.skip_over_closing = false;
  EXPECT_EQ(EvaluateBracketInput(U'」', false, {{}, U'」', true}, options).type,
            BracketPairingActionType::kInsertLiteral);
}

TEST(BracketPairingTest, BackspaceDeletesOnlyEmptyMatchingPair) {
  auto options = Enabled();
  const auto action = EvaluateBracketBackspace(false, {U'「', U'」', true}, options);
  EXPECT_EQ(action.type, BracketPairingActionType::kDeletePair);
  EXPECT_EQ(action.open, U'「');
  EXPECT_EQ(action.close, U'」');
  for (auto before : {std::optional<char32_t>{}, std::optional<char32_t>{U'あ'},
                      std::optional<char32_t>{U'('}, std::optional<char32_t>{0xdfff}}) {
    EXPECT_EQ(EvaluateBracketBackspace(false, {before, U'」', true}, options).type,
              BracketPairingActionType::kPassThrough);
  }
  options.backspace_deletes_pair = false;
  EXPECT_EQ(EvaluateBracketBackspace(false, {U'「', U'」', true}, options).type,
            BracketPairingActionType::kPassThrough);
}

TEST(BracketPairingTest, AlnumOptionControlsInsertionSkippingAndDeletion) {
  auto options = Enabled();
  EXPECT_EQ(EvaluateBracketInput(U'[', true, {{}, {}, true}, options).type,
            BracketPairingActionType::kInsertPair);
  options.enabled_in_alnum_mode = false;
  for (char32_t cp : U"[]") {
    EXPECT_EQ(EvaluateBracketInput(cp, true, {U'[', U']', true}, options).type,
              BracketPairingActionType::kPassThrough);
  }
  EXPECT_EQ(EvaluateBracketBackspace(true, {U'[', U']', true}, options).type,
            BracketPairingActionType::kPassThrough);
  EXPECT_EQ(EvaluateBracketInput(U'「', false, {{}, {}, true}, options).type,
            BracketPairingActionType::kInsertPair);
}

TEST(BracketPairingTest, QuotesAnglesAndOrdinaryCharactersAreExcluded) {
  for (char32_t cp : U"\"'`<>あa。") {
    EXPECT_EQ(EvaluateBracketInput(cp, false, {{}, {}, true}, Enabled()).type,
              BracketPairingActionType::kPassThrough);
  }
}

TEST(BracketSettingsTest, MissingInvalidAndUnrelatedFieldsKeepDefaults) {
  for (const auto json : {"", "{", "[]", "null", "{}", R"({"unrelated":true})",
                          R"({"bracketPairing":"true","bracketPairingTrigger":false})"}) {
    const auto settings = ParseBracketSettings(json);
    EXPECT_FALSE(settings.pairing.enabled);
    EXPECT_TRUE(settings.pairing.skip_over_closing);
    EXPECT_TRUE(settings.pairing.backspace_deletes_pair);
    EXPECT_TRUE(settings.pairing.enabled_in_alnum_mode);
    EXPECT_EQ(settings.trigger, BracketPairingTrigger::Immediate);
  }
}

TEST(BracketSettingsTest, ReadsEveryTipOwnedKeyFromSharedDocument) {
  const auto settings = ParseBracketSettings(R"({
    "inputMode":"alnum_full", "bracketPairing":true,
    "bracketPairingTrigger":"composition", "bracketSkipOverClosing":false,
    "bracketBackspaceDeletesPair":false, "bracketPairingInAlnumMode":false,
    "aiBackend":"none"
  })");
  EXPECT_TRUE(settings.pairing.enabled);
  EXPECT_FALSE(settings.pairing.skip_over_closing);
  EXPECT_FALSE(settings.pairing.backspace_deletes_pair);
  EXPECT_FALSE(settings.pairing.enabled_in_alnum_mode);
  EXPECT_EQ(settings.trigger, BracketPairingTrigger::Composition);
  EXPECT_EQ(settings.input_mode, BracketInputMode::AlnumFull);
  EXPECT_EQ(ParseBracketSettings(R"({"inputMode":"alnum_half"})").input_mode,
            BracketInputMode::AlnumHalf);
  EXPECT_EQ(ParseBracketSettings(R"({"inputMode":"invalid"})").input_mode,
            BracketInputMode::Hiragana);
}
}  // namespace
