#include "azookey/core/KanaInput.h"

#include <array>
#include <cstddef>

#include "azookey/core/Utf8.h"

namespace azookey::core {
namespace {

constexpr std::string_view kDakutenMark = "゛";     // U+309B
constexpr std::string_view kHandakutenMark = "゜";  // U+309C

struct KanaKey {
  uint8_t scan_code;
  std::string_view plain;
  std::string_view shifted;
};

// docs/input-style-spec.md section 4.2, ordered by scan code.
constexpr std::array<KanaKey, 48> kKanaJisTable = {{
    {0x02, "ぬ", "ぬ"}, {0x03, "ふ", "ふ"}, {0x04, "あ", "ぁ"}, {0x05, "う", "ぅ"},
    {0x06, "え", "ぇ"}, {0x07, "お", "ぉ"}, {0x08, "や", "ゃ"}, {0x09, "ゆ", "ゅ"},
    {0x0A, "よ", "ょ"}, {0x0B, "わ", "を"}, {0x0C, "ほ", "ほ"}, {0x0D, "へ", "へ"},
    {0x10, "た", "た"}, {0x11, "て", "て"}, {0x12, "い", "ぃ"}, {0x13, "す", "す"},
    {0x14, "か", "か"}, {0x15, "ん", "ん"}, {0x16, "な", "な"}, {0x17, "に", "に"},
    {0x18, "ら", "ら"}, {0x19, "せ", "せ"}, {0x1A, "゛", "゛"}, {0x1B, "゜", "「"},
    {0x1E, "ち", "ち"}, {0x1F, "と", "と"}, {0x20, "し", "し"}, {0x21, "は", "は"},
    {0x22, "き", "き"}, {0x23, "く", "く"}, {0x24, "ま", "ま"}, {0x25, "の", "の"},
    {0x26, "り", "り"}, {0x27, "れ", "れ"}, {0x28, "け", "け"}, {0x2B, "む", "」"},
    {0x2C, "つ", "っ"}, {0x2D, "さ", "さ"}, {0x2E, "そ", "そ"}, {0x2F, "ひ", "ひ"},
    {0x30, "こ", "こ"}, {0x31, "み", "み"}, {0x32, "も", "も"}, {0x33, "ね", "、"},
    {0x34, "る", "。"}, {0x35, "め", "・"}, {0x73, "ろ", "ろ"}, {0x7D, "ー", "ー"},
}};

struct VoicedPair {
  std::string_view base;
  std::string_view dakuten;
  std::string_view handakuten;  // empty when the base has no semi-voiced form
};

constexpr std::array<VoicedPair, 21> kVoicedPairs = {{
    {"う", "ゔ", ""},   {"か", "が", ""},   {"き", "ぎ", ""},   {"く", "ぐ", ""},
    {"け", "げ", ""},   {"こ", "ご", ""},   {"さ", "ざ", ""},   {"し", "じ", ""},
    {"す", "ず", ""},   {"せ", "ぜ", ""},   {"そ", "ぞ", ""},   {"た", "だ", ""},
    {"ち", "ぢ", ""},   {"つ", "づ", ""},   {"て", "で", ""},   {"と", "ど", ""},
    {"は", "ば", "ぱ"}, {"ひ", "び", "ぴ"}, {"ふ", "ぶ", "ぷ"}, {"へ", "べ", "ぺ"},
    {"ほ", "ぼ", "ぽ"},
}};

// The last code point of `text` as a UTF-8 slice; empty for empty or malformed text.
std::string_view LastCodePoint(std::string_view text) {
  size_t start = text.size();
  while (start > 0 && (static_cast<unsigned char>(text[start - 1]) & 0xC0) == 0x80) --start;
  if (start == 0) return {};
  --start;
  size_t offset = start;
  char32_t codepoint = 0;
  if (!DecodeNextUtf8(text, offset, codepoint) || offset != text.size()) return {};
  return text.substr(start);
}

}  // namespace

std::optional<std::string_view> LookupKanaJis(uint8_t scan_code, bool extended, bool shift) {
  if (extended) return std::nullopt;
  for (const KanaKey& key : kKanaJisTable) {
    if (key.scan_code == scan_code) return shift ? key.shifted : key.plain;
  }
  return std::nullopt;
}

std::optional<VoicingMark> AsVoicingMark(std::string_view kana) {
  if (kana == kDakutenMark) return VoicingMark::kDakuten;
  if (kana == kHandakutenMark) return VoicingMark::kHandakuten;
  return std::nullopt;
}

VoicingResult ApplyVoicingMark(std::string_view reading_before_cursor, VoicingMark mark) {
  const std::string_view previous = LastCodePoint(reading_before_cursor);
  for (const VoicedPair& pair : kVoicedPairs) {
    if (pair.base != previous) continue;
    const std::string_view combined =
        mark == VoicingMark::kDakuten ? pair.dakuten : pair.handakuten;
    if (combined.empty()) break;
    return VoicingResult{previous.size(), std::string(combined)};
  }
  return VoicingResult{0,
                       std::string(mark == VoicingMark::kDakuten ? kDakutenMark : kHandakutenMark)};
}

}  // namespace azookey::core
