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
  static CustomRomajiParseResult Parse(std::string_view tsv);
};

}  // namespace azookey::core
