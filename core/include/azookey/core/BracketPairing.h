#pragma once

#include <map>
#include <optional>
#include <string_view>
#include <vector>

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

struct BracketTable {
  std::map<char32_t, char32_t> pairs;
  std::map<char32_t, char32_t> closing;
};

struct BracketTableParseResult {
  BracketTable table;
  std::vector<size_t> invalid_lines;
};

const BracketTable& BuiltinBracketTable();
BracketTableParseResult ParseBracketTable(std::string_view tsv);

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

std::optional<BracketPair> LookupBracketPair(char32_t codepoint,
                                             const BracketTable& table = BuiltinBracketTable());
std::optional<BracketPair> LookupClosingBracket(char32_t codepoint,
                                                const BracketTable& table = BuiltinBracketTable());

BracketPairingAction EvaluateBracketInput(char32_t codepoint, bool alnum_mode,
                                          const EditContextHint& hint,
                                          const BracketPairingOptions& options,
                                          const BracketTable& table = BuiltinBracketTable());
BracketPairingAction EvaluateBracketBackspace(bool alnum_mode, const EditContextHint& hint,
                                              const BracketPairingOptions& options,
                                              const BracketTable& table = BuiltinBracketTable());

}  // namespace azookey::core
