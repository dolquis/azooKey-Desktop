#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::core {

struct CustomRomajiRule {
  std::string output;
  size_t consume = 0;
};

using CustomRomajiTable = std::map<std::string, CustomRomajiRule, std::less<>>;

struct CustomRomajiParseResult {
  std::shared_ptr<const CustomRomajiTable> table;
  // One-based line numbers for callers to include in warning logs.
  std::vector<size_t> invalid_lines;
};

class CustomRomajiLoader {
 public:
  // Parse UTF-8 TSV bytes. File reading and warning logging belong to the caller.
  //
  // `case_sensitive_rules` false (the default) folds the ASCII letters of every rule key to
  // lowercase, so Shift does not matter when the rules are looked up. True keeps the case of the
  // keys (docs/legacy-parity-spec.md section 5.1): a capital rule then matches only Shift-typed
  // letters, so the caller must feed the letters in the case Shift gives them.
  static CustomRomajiParseResult Parse(std::string_view tsv, bool case_sensitive_rules = false);
};

}  // namespace azookey::core
