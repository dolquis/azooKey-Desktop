#include "azookey/core/BracketPairing.h"

namespace azookey::core {
namespace {
bool IsWhitespace(char32_t cp) {
  return (cp >= 0x9 && cp <= 0xd) || cp == U' ' || cp == 0x85 || cp == 0xa0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f ||
         cp == 0x205f || cp == 0x3000;
}
}  // namespace

BracketPairingAction EvaluateBracketInput(char32_t codepoint, bool alnum_mode,
                                          const EditContextHint& hint,
                                          const BracketPairingOptions& options,
                                          const BracketTable& table) {
  if (!options.enabled || (alnum_mode && !options.enabled_in_alnum_mode)) return {};
  if (const auto pair = LookupBracketPair(codepoint, table)) {
    const bool symmetric = pair->open == pair->close;
    if (symmetric && !options.symmetric_quote_pairing) return {};
    if (hint.selection_collapsed == false && options.wrap_selection)
      return {BracketPairingActionType::kWrapSelection, pair->open, pair->close};
    if (hint.selection_collapsed != true) {
      return {BracketPairingActionType::kInsertLiteral, codepoint};
    }
    if (symmetric) {
      if (options.skip_over_closing && hint.char_after == codepoint)
        return {BracketPairingActionType::kSkipClosing, {}, codepoint};
      if (hint.char_before && !IsWhitespace(*hint.char_before) &&
          (*hint.char_before == codepoint || !LookupBracketPair(*hint.char_before, table)))
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
  if (!pair || (pair->open == pair->close && !options.symmetric_quote_pairing) ||
      hint.char_after != pair->close)
    return {};
  return {BracketPairingActionType::kDeletePair, pair->open, pair->close};
}
}  // namespace azookey::core
