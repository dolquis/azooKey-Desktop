#pragma once

#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include "azookey/core/EditContextHint.h"

namespace azookey::core {

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
  kWrapSelection,
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
  bool symmetric_quote_pairing{false};
  bool wrap_selection{false};
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
