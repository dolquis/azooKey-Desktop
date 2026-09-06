#pragma once

#include <optional>

namespace azookey::core {

// Only the TIP reads the document. Unknown selection means a failed read, not
// an empty document; it must never authorize a destructive action.
struct EditContextHint {
  std::optional<char32_t> char_before;
  std::optional<char32_t> char_after;
  std::optional<bool> selection_collapsed;
};

struct BracketPair {
  char32_t open;
  char32_t close;
};

enum class BracketPairingActionType {
  kPassThrough,
  kInsertPair,
  kInsertLiteral,
  kSkipClosing,
  kDeletePair,
};

struct BracketPairingAction {
  BracketPairingActionType type{BracketPairingActionType::kPassThrough};
  char32_t open{};
  char32_t close{};
};

struct BracketPairingOptions {
  bool enabled{false};
  bool skip_over_closing{true};
  bool backspace_deletes_pair{true};
  bool enabled_in_alnum_mode{true};
};

std::optional<BracketPair> LookupBracketPair(char32_t codepoint);
std::optional<BracketPair> LookupClosingBracket(char32_t codepoint);

BracketPairingAction EvaluateBracketInput(char32_t codepoint, bool alnum_mode,
                                          const EditContextHint& hint,
                                          const BracketPairingOptions& options);
BracketPairingAction EvaluateBracketBackspace(bool alnum_mode, const EditContextHint& hint,
                                              const BracketPairingOptions& options);

}  // namespace azookey::core
