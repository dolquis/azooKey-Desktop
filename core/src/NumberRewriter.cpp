#include "azookey/core/NumberRewriter.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace azookey::core {
namespace {

constexpr std::array<const char*, 10> kKanjiDigits = {
    "", "一", "二", "三", "四", "五", "六", "七", "八", "九",
};

constexpr std::array<const char*, 10> kDaijiDigits = {
    "", "壱", "弐", "参", "肆", "伍", "陸", "漆", "捌", "玖",
};

constexpr std::array<const char*, 4> kKanjiSmallUnits = {
    "", "十", "百", "千",
};

constexpr std::array<const char*, 4> kDaijiSmallUnits = {
    "", "拾", "百", "千",
};

constexpr std::array<const char*, 5> kLargeUnits = {
    "", "万", "億", "兆", "京",
};

constexpr std::array<const char*, 13> kRomanUpper = {
    "", "Ⅰ", "Ⅱ", "Ⅲ", "Ⅳ", "Ⅴ", "Ⅵ", "Ⅶ", "Ⅷ", "Ⅸ", "Ⅹ", "Ⅺ", "Ⅻ",
};

constexpr std::array<const char*, 13> kRomanLower = {
    "", "ⅰ", "ⅱ", "ⅲ", "ⅳ", "ⅴ", "ⅵ", "ⅶ", "ⅷ", "ⅸ", "ⅹ", "ⅺ", "ⅻ",
};

constexpr std::array<const char*, 51> kCircledNumbers = {
    "⓪", "①", "②", "③", "④", "⑤", "⑥", "⑦", "⑧", "⑨", "⑩",
    "⑪", "⑫", "⑬", "⑭", "⑮", "⑯", "⑰", "⑱", "⑲", "⑳",
    "㉑", "㉒", "㉓", "㉔", "㉕", "㉖", "㉗", "㉘", "㉙", "㉚",
    "㉛", "㉜", "㉝", "㉞", "㉟", "㊱", "㊲", "㊳", "㊴", "㊵",
    "㊶", "㊷", "㊸", "㊹", "㊺", "㊻", "㊼", "㊽", "㊾", "㊿",
};

bool IsAsciiDecimal(const std::string& text) {
  if (text.empty()) return false;
  for (const unsigned char ch : text) {
    if (ch < '0' || ch > '9') return false;
  }
  return true;
}

bool ParseUint64(const std::string& text, std::uint64_t* value) {
  std::uint64_t parsed = 0;
  for (const unsigned char ch : text) {
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
  }
  *value = parsed;
  return true;
}

std::string ConvertSegment(std::uint16_t segment, bool daiji) {
  std::string result;
  const auto& digits = daiji ? kDaijiDigits : kKanjiDigits;
  const auto& units = daiji ? kDaijiSmallUnits : kKanjiSmallUnits;

  for (int pos = 3; pos >= 0; --pos) {
    std::uint16_t divisor = 1;
    for (int i = 0; i < pos; ++i) divisor *= 10;
    const auto digit = static_cast<std::uint16_t>((segment / divisor) % 10);
    if (digit == 0) continue;
    if (!(digit == 1 && pos > 0 && !daiji)) {
      result += digits[digit];
    }
    result += units[pos];
  }
  return result;
}

std::string ConvertJapaneseNumber(std::uint64_t value, bool daiji) {
  if (value == 0) return "零";

  std::array<std::uint16_t, kLargeUnits.size()> segments{};
  std::size_t count = 0;
  while (value > 0 && count < segments.size()) {
    segments[count++] = static_cast<std::uint16_t>(value % 10000);
    value /= 10000;
  }
  if (value != 0) return "";

  std::string result;
  for (std::size_t index = count; index > 0; --index) {
    const std::size_t segment_index = index - 1;
    if (segments[segment_index] == 0) continue;
    result += ConvertSegment(segments[segment_index], daiji);
    result += kLargeUnits[segment_index];
  }
  return result;
}

std::string ToBase(std::uint64_t value, std::uint64_t base) {
  constexpr char kDigits[] = "0123456789abcdef";
  if (value == 0) return "0";
  std::string reversed;
  while (value > 0) {
    reversed.push_back(kDigits[value % base]);
    value /= base;
  }
  return std::string(reversed.rbegin(), reversed.rend());
}

void AddCandidate(std::vector<Candidate>& candidates,
                  const std::string& reading,
                  std::string surface,
                  std::string description,
                  std::string debug_tag) {
  if (surface.empty() || surface == reading) return;
  for (const auto& candidate : candidates) {
    if (candidate.surface == surface) return;
  }

  Candidate candidate;
  candidate.surface = std::move(surface);
  candidate.reading = reading;
  candidate.score = 0.05;
  candidate.source = CandidateSource::Heuristic;
  candidate.debug_info = "number-rewriter:" + std::move(debug_tag);
  candidate.description = std::move(description);
  candidates.push_back(std::move(candidate));
}

}  // namespace

std::vector<Candidate> ExpandNumberCandidates(const std::string& reading) {
  if (!IsAsciiDecimal(reading)) return {};

  std::uint64_t value = 0;
  if (!ParseUint64(reading, &value)) return {};

  std::vector<Candidate> candidates;
  AddCandidate(candidates, reading, ConvertJapaneseNumber(value, false), "漢数字", "kanji");
  AddCandidate(candidates, reading, ConvertJapaneseNumber(value, true), "大字", "daiji");

  if (value < kCircledNumbers.size()) {
    AddCandidate(candidates, reading, kCircledNumbers[value], "丸数字", "circled");
  }
  if (value > 0 && value < kRomanUpper.size()) {
    AddCandidate(candidates, reading, kRomanUpper[value], "ローマ数字（大文字）", "roman-upper");
    AddCandidate(candidates, reading, kRomanLower[value], "ローマ数字（小文字）", "roman-lower");
  }

  AddCandidate(candidates, reading, "0x" + ToBase(value, 16), "16進数", "hex");
  AddCandidate(candidates, reading, "0" + ToBase(value, 8), "8進数", "octal");
  AddCandidate(candidates, reading, "0b" + ToBase(value, 2), "2進数", "binary");
  return candidates;
}

}  // namespace azookey::core
