#include <gtest/gtest.h>

#include "azookey/core/BracketPairing.h"

namespace {
using namespace azookey::core;

BracketPairingOptions Enabled() {
  BracketPairingOptions options;
  options.enabled = true;
  return options;
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
}  // namespace
