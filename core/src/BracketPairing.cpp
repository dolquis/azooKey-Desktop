#include "azookey/core/BracketPairing.h"

namespace azookey::core {

BracketPairingAction EvaluateBracketInput(char32_t codepoint, bool alnum_mode,
                                          const EditContextHint& hint,
                                          const BracketPairingOptions& options,
                                          const BracketTable& table) {
  if (!options.enabled || (alnum_mode && !options.enabled_in_alnum_mode)) return {};
  if (const auto pair = LookupBracketPair(codepoint, table)) {
    if (pair->open == pair->close) return {};  // Symmetric pairs require the separate opt-in.
    if (hint.selection_collapsed != true) {
      return {BracketPairingActionType::kInsertLiteral, codepoint};
    }
    return {BracketPairingActionType::kInsertPair, pair->open, pair->close};
  }
  if (LookupClosingBracket(codepoint, table)) {
    if (options.skip_over_closing && hint.selection_collapsed == true &&
        hint.char_after == codepoint) {
      return {BracketPairingActionType::kSkipClosing, {}, codepoint};
    }
    return {BracketPairingActionType::kInsertLiteral, codepoint};
  }
  return {};
}

BracketPairingAction EvaluateBracketBackspace(bool alnum_mode, const EditContextHint& hint,
                                              const BracketPairingOptions& options,
                                              const BracketTable& table) {
  if (!options.enabled || (alnum_mode && !options.enabled_in_alnum_mode) ||
      !options.backspace_deletes_pair || hint.selection_collapsed != true || !hint.char_before) {
    return {};
  }
  const auto pair = LookupBracketPair(*hint.char_before, table);
  if (!pair || pair->open == pair->close || hint.char_after != pair->close) return {};
  return {BracketPairingActionType::kDeletePair, pair->open, pair->close};
}
}  // namespace azookey::core
