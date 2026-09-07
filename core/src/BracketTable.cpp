#include <array>

#include "azookey/core/BracketPairing.h"
#include "azookey/core/Utf8.h"

namespace azookey::core {
namespace {
constexpr std::array<BracketPair, 17> kBracketPairs{{
    {U'（', U'）'},
    {U'「', U'」'},
    {U'『', U'』'},
    {U'【', U'】'},
    {U'〔', U'〕'},
    {U'［', U'］'},
    {U'｛', U'｝'},
    {U'〈', U'〉'},
    {U'《', U'》'},
    {U'“', U'”'},
    {U'‘', U'’'},
    {U'(', U')'},
    {U'[', U']'},
    {U'{', U'}'},
    {U'"', U'"'},
    {U'\'', U'\''},
    {U'`', U'`'},
}};
}  // namespace

const BracketTable& BuiltinBracketTable() {
  static const BracketTable table = [] {
    BracketTable value;
    for (const auto& pair : kBracketPairs) {
      value.pairs.emplace(pair.open, pair.close);
      value.closing.emplace(pair.close, pair.open);
    }
    return value;
  }();
  return table;
}

std::optional<BracketPair> LookupBracketPair(char32_t codepoint, const BracketTable& table) {
  const auto found = table.pairs.find(codepoint);
  if (found == table.pairs.end()) return std::nullopt;
  return BracketPair{codepoint, found->second};
}

std::optional<BracketPair> LookupClosingBracket(char32_t codepoint, const BracketTable& table) {
  const auto found = table.closing.find(codepoint);
  if (found == table.closing.end()) return std::nullopt;
  return BracketPair{found->second, codepoint};
}

BracketTableParseResult ParseBracketTable(std::string_view tsv) {
  BracketTableParseResult result{BuiltinBracketTable(), {}};
  if (tsv.starts_with("\xef\xbb\xbf")) tsv.remove_prefix(3);
  size_t line_number = 0;
  while (!tsv.empty()) {
    ++line_number;
    const auto end = tsv.find('\n');
    auto line = tsv.substr(0, end);
    tsv.remove_prefix(end == std::string_view::npos ? tsv.size() : end + 1);
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line.empty() || line.starts_with('#')) continue;
    const auto first = line.find('\t');
    const auto second = first == std::string_view::npos ? first : line.find('\t', first + 1);
    const auto open_text = line.substr(0, first);
    const auto close_text =
        first == std::string_view::npos
            ? std::string_view{}
            : line.substr(first + 1,
                          second == std::string_view::npos ? second : second - first - 1);
    const auto flags =
        second == std::string_view::npos ? std::string_view{} : line.substr(second + 1);
    const auto scalar = [](std::string_view text, char32_t& cp) {
      size_t offset = 0;
      return DecodeNextUtf8(text, offset, cp) && offset == text.size() && cp > 0x20 &&
             cp <= 0xffff && !(cp >= 0x7f && cp <= 0x9f);
    };
    char32_t open{}, close{};
    if (!scalar(open_text, open) || !scalar(close_text, close) ||
        (!flags.empty() && flags != "off")) {
      result.invalid_lines.push_back(line_number);
      continue;
    }
    if (flags == "off")
      result.table.pairs.erase(open);
    else
      result.table.pairs[open] = close;
  }
  result.table.closing.clear();
  for (const auto& [open, close] : result.table.pairs) result.table.closing.emplace(close, open);
  return result;
}
}  // namespace azookey::core
