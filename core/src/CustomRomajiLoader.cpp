#include "azookey/core/CustomRomajiLoader.h"

#include <charconv>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

#include "azookey/core/Utf8.h"

namespace azookey::core {
namespace {

bool ValidInput(std::string_view input) {
  if (input.empty() || input.size() > 8) return false;
  for (unsigned char byte : input) {
    if (byte > 0x7f) return false;
  }
  return true;
}

bool ValidOutput(std::string_view output) {
  size_t units = 0;
  size_t offset = 0;
  while (offset < output.size()) {
    char32_t codepoint = 0;
    if (!DecodeNextUtf8(output, offset, codepoint)) return false;
    units += codepoint > 0xffff ? 2 : 1;
    if (units > 8) return false;
  }
  return true;
}

bool ParseLine(std::string_view line, CustomRomajiTable& table) {
  const size_t first_tab = line.find('\t');
  if (first_tab == std::string_view::npos) return false;
  const size_t second_tab = line.find('\t', first_tab + 1);
  if (second_tab != std::string_view::npos &&
      line.find('\t', second_tab + 1) != std::string_view::npos)
    return false;

  const std::string_view input = line.substr(0, first_tab);
  const std::string_view output =
      line.substr(first_tab + 1, second_tab == std::string_view::npos ? std::string_view::npos
                                                                      : second_tab - first_tab - 1);
  if (!ValidInput(input) || !ValidOutput(output)) return false;

  size_t consume = input.size();
  if (second_tab != std::string_view::npos) {
    const std::string_view field = line.substr(second_tab + 1);
    if (field.empty()) return false;
    const auto result = std::from_chars(field.data(), field.data() + field.size(), consume);
    if (result.ec != std::errc{} || result.ptr != field.data() + field.size() || consume == 0 ||
        consume > input.size())
      return false;
  }
  std::string normalized_input(input);
  for (char& c : normalized_input) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  table[std::move(normalized_input)] = CustomRomajiRule{std::string(output), consume};
  return true;
}

}  // namespace

CustomRomajiParseResult CustomRomajiLoader::Parse(std::string_view tsv) {
  if (tsv.substr(0, 3) == "\xef\xbb\xbf") tsv.remove_prefix(3);

  auto table = std::make_shared<CustomRomajiTable>();
  std::vector<size_t> invalid_lines;
  size_t line_number = 1;
  while (!tsv.empty()) {
    const size_t end = tsv.find('\n');
    std::string_view line = tsv.substr(0, end);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty() && line.front() != '#' && !ParseLine(line, *table)) {
      invalid_lines.push_back(line_number);
    }
    if (end == std::string_view::npos) break;
    tsv.remove_prefix(end + 1);
    ++line_number;
  }
  if (table->empty()) table.reset();
  return {std::move(table), std::move(invalid_lines)};
}

}  // namespace azookey::core
