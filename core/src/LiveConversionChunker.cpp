#include "azookey/core/LiveConversionChunker.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azookey::core {
namespace {

struct Utf8Unit {
  char32_t code_point{};
  size_t offset{};
  size_t byte_length{1};
};

struct DecodeResult {
  char32_t code_point{};
  size_t byte_length{1};
};

bool IsContinuation(unsigned char ch) { return (ch & 0xc0) == 0x80; }

DecodeResult DecodeUtf8At(std::string_view text, size_t offset) {
  const auto remaining = text.size() - offset;
  const auto b0 = static_cast<unsigned char>(text[offset]);
  if (b0 < 0x80) {
    return DecodeResult{b0, 1};
  }
  if ((b0 & 0xe0) == 0xc0 && remaining >= 2) {
    const auto b1 = static_cast<unsigned char>(text[offset + 1]);
    if (IsContinuation(b1)) {
      const char32_t cp = ((b0 & 0x1f) << 6) | (b1 & 0x3f);
      if (cp >= 0x80) return DecodeResult{cp, 2};
    }
  }
  if ((b0 & 0xf0) == 0xe0 && remaining >= 3) {
    const auto b1 = static_cast<unsigned char>(text[offset + 1]);
    const auto b2 = static_cast<unsigned char>(text[offset + 2]);
    if (IsContinuation(b1) && IsContinuation(b2)) {
      const char32_t cp = ((b0 & 0x0f) << 12) | ((b1 & 0x3f) << 6) | (b2 & 0x3f);
      if (cp >= 0x800 && (cp < 0xd800 || cp > 0xdfff)) return DecodeResult{cp, 3};
    }
  }
  if ((b0 & 0xf8) == 0xf0 && remaining >= 4) {
    const auto b1 = static_cast<unsigned char>(text[offset + 1]);
    const auto b2 = static_cast<unsigned char>(text[offset + 2]);
    const auto b3 = static_cast<unsigned char>(text[offset + 3]);
    if (IsContinuation(b1) && IsContinuation(b2) && IsContinuation(b3)) {
      const char32_t cp =
          ((b0 & 0x07) << 18) | ((b1 & 0x3f) << 12) | ((b2 & 0x3f) << 6) | (b3 & 0x3f);
      if (cp >= 0x10000 && cp <= 0x10ffff) return DecodeResult{cp, 4};
    }
  }
  return DecodeResult{0xfffd, 1};
}

std::vector<Utf8Unit> DecodeUtf8Units(std::string_view text) {
  std::vector<Utf8Unit> units;
  for (size_t offset = 0; offset < text.size();) {
    const auto decoded = DecodeUtf8At(text, offset);
    units.push_back(Utf8Unit{decoded.code_point, offset, decoded.byte_length});
    offset += decoded.byte_length;
  }
  return units;
}

LiveConversionChunkKind KindFor(char32_t code_point) {
  return IsJapaneseForLiveConversion(code_point) ? LiveConversionChunkKind::Convert
                                                 : LiveConversionChunkKind::Verbatim;
}

std::vector<LiveConversionChunk> GroupUnitRange(std::string_view text,
                                                const std::vector<Utf8Unit>& units,
                                                size_t begin_unit, size_t end_unit) {
  std::vector<LiveConversionChunk> chunks;
  if (begin_unit >= end_unit) return chunks;

  LiveConversionChunk current;
  current.kind = KindFor(units[begin_unit].code_point);
  current.byte_offset = units[begin_unit].offset;
  size_t current_end = current.byte_offset;

  for (size_t index = begin_unit; index < end_unit; ++index) {
    const auto kind = KindFor(units[index].code_point);
    const auto unit_end = units[index].offset + units[index].byte_length;
    if (index != begin_unit && kind != current.kind) {
      current.text =
          std::string(text.substr(current.byte_offset, current_end - current.byte_offset));
      chunks.push_back(std::move(current));
      current = LiveConversionChunk{};
      current.kind = kind;
      current.byte_offset = units[index].offset;
    }
    current_end = unit_end;
  }

  current.text = std::string(text.substr(current.byte_offset, current_end - current.byte_offset));
  chunks.push_back(std::move(current));
  return chunks;
}

bool SameUtf8Unit(std::string_view lhs, const Utf8Unit& lhs_unit, std::string_view rhs,
                  const Utf8Unit& rhs_unit) {
  if (lhs_unit.byte_length != rhs_unit.byte_length) return false;
  return lhs.substr(lhs_unit.offset, lhs_unit.byte_length) ==
         rhs.substr(rhs_unit.offset, rhs_unit.byte_length);
}

size_t CommonPrefixUnits(std::string_view previous_text,
                         const std::vector<Utf8Unit>& previous_units, std::string_view current_text,
                         const std::vector<Utf8Unit>& current_units) {
  const size_t limit = std::min(previous_units.size(), current_units.size());
  size_t count = 0;
  while (count < limit &&
         SameUtf8Unit(previous_text, previous_units[count], current_text, current_units[count])) {
    ++count;
  }
  return count;
}

size_t CommonSuffixUnits(std::string_view previous_text,
                         const std::vector<Utf8Unit>& previous_units, std::string_view current_text,
                         const std::vector<Utf8Unit>& current_units, size_t prefix_units) {
  size_t count = 0;
  while (count < previous_units.size() - prefix_units &&
         count < current_units.size() - prefix_units) {
    const auto& previous_unit = previous_units[previous_units.size() - 1 - count];
    const auto& current_unit = current_units[current_units.size() - 1 - count];
    if (!SameUtf8Unit(previous_text, previous_unit, current_text, current_unit)) break;
    ++count;
  }
  return count;
}

size_t UnitOffsetOrEnd(const std::vector<Utf8Unit>& units, size_t unit_index, size_t text_size) {
  return unit_index < units.size() ? units[unit_index].offset : text_size;
}

}  // namespace

bool IsJapaneseForLiveConversion(char32_t code_point) {
  if (code_point >= 0x3041 && code_point <= 0x3096) return true;    // Hiragana.
  if (code_point >= 0x309d && code_point <= 0x309f) return true;    // Hiragana iteration marks.
  if (code_point >= 0x30a1 && code_point <= 0x30fa) return true;    // Katakana.
  if (code_point >= 0x30fd && code_point <= 0x30ff) return true;    // Katakana iteration marks.
  if (code_point >= 0x31f0 && code_point <= 0x31ff) return true;    // Katakana phonetic extensions.
  if (code_point >= 0xff66 && code_point <= 0xff9f) return true;    // Halfwidth Katakana.
  if (code_point >= 0x3400 && code_point <= 0x4dbf) return true;    // CJK extension A.
  if (code_point >= 0x4e00 && code_point <= 0x9fff) return true;    // CJK unified.
  if (code_point >= 0xf900 && code_point <= 0xfaff) return true;    // CJK compatibility.
  if (code_point >= 0x20000 && code_point <= 0x2ebef) return true;  // CJK extensions.
  return code_point == 0x3005 || code_point == 0x3006 || code_point == 0x3007 ||
         code_point == 0x30fc;
}

std::vector<LiveConversionChunk> GroupLiveConversionChunks(std::string_view text) {
  const auto units = DecodeUtf8Units(text);
  return GroupUnitRange(text, units, 0, units.size());
}

LiveConversionChunkPlan ComputeLiveConversionChunkPlan(std::string_view previous_text,
                                                       std::string_view current_text) {
  const auto previous_units = DecodeUtf8Units(previous_text);
  const auto current_units = DecodeUtf8Units(current_text);
  const size_t prefix_units =
      CommonPrefixUnits(previous_text, previous_units, current_text, current_units);
  const size_t suffix_units =
      CommonSuffixUnits(previous_text, previous_units, current_text, current_units, prefix_units);

  LiveConversionChunkPlan plan;
  plan.chunks = GroupUnitRange(current_text, current_units, 0, current_units.size());
  plan.unchanged_prefix_bytes = UnitOffsetOrEnd(current_units, prefix_units, current_text.size());
  plan.unchanged_suffix_bytes =
      current_text.size() -
      UnitOffsetOrEnd(current_units, current_units.size() - suffix_units, current_text.size());
  plan.replace_old_begin_byte = UnitOffsetOrEnd(previous_units, prefix_units, previous_text.size());
  plan.replace_old_end_byte =
      UnitOffsetOrEnd(previous_units, previous_units.size() - suffix_units, previous_text.size());
  plan.replace_new_begin_byte = UnitOffsetOrEnd(current_units, prefix_units, current_text.size());
  plan.replace_new_end_byte =
      UnitOffsetOrEnd(current_units, current_units.size() - suffix_units, current_text.size());
  plan.replacement_chunks = GroupUnitRange(current_text, current_units, prefix_units,
                                           current_units.size() - suffix_units);
  return plan;
}

}  // namespace azookey::core
