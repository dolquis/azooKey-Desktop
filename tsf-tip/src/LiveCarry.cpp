#include "azookey/tsf/LiveCarry.h"

#include <algorithm>
#include <cstdint>
#include <optional>

#include "azookey/core/Utf8.h"

namespace azookey::tsf {
namespace {

struct Codepoint {
  char32_t value{};
  std::string text;
};

std::vector<Codepoint> SplitCodepoints(std::string_view text) {
  std::vector<Codepoint> out;
  size_t offset = 0;
  while (offset < text.size()) {
    const size_t begin = offset;
    char32_t value = 0;
    (void)core::DecodeNextUtf8(text, offset, value);
    out.push_back({value, std::string(text.substr(begin, offset - begin))});
  }
  return out;
}

// The kana a reading spells the same way: hiragana, katakana as hiragana, and
// the long vowel mark. ヵ and ヶ are read as か, け or こ, so they are not.
std::optional<char32_t> KanaKey(char32_t c) {
  if (c >= 0x3041 && c <= 0x3094) return c;
  if (c >= 0x30A1 && c <= 0x30F4) return c - 0x60;
  if (c == 0x30FC) return c;
  return std::nullopt;
}

// Longer segments are carried whole; the split is a per-answer quadratic count.
constexpr size_t kMaxAlignedCodepoints = 64;

// Splits one converted segment into its kana and the runs of other characters
// between them, each run reading one or more characters, when exactly one
// such split matches the reading. Otherwise none.
std::vector<LivePiece> AlignSegment(std::string_view reading, std::string_view surface) {
  const auto typed = SplitCodepoints(reading);
  const auto shown = SplitCodepoints(surface);
  struct Unit {
    std::string text;
    std::optional<char32_t> kana;
  };
  std::vector<Unit> units;
  for (const auto& c : shown) {
    const auto key = KanaKey(c.value);
    if (key || units.empty() || units.back().kana)
      units.push_back({c.text, key});
    else
      units.back().text += c.text;
  }
  const size_t unit_count = units.size();
  const size_t typed_count = typed.size();
  if (unit_count == 0 || typed_count == 0 || unit_count > kMaxAlignedCodepoints ||
      typed_count > kMaxAlignedCodepoints)
    return {};
  // ways[i][j]: the splits of units[i..] over typed[j..], counted up to 2.
  std::vector<uint8_t> ways((unit_count + 1) * (typed_count + 1), 0);
  const auto at = [&](size_t i, size_t j) -> uint8_t& { return ways[i * (typed_count + 1) + j]; };
  at(unit_count, typed_count) = 1;
  for (size_t i = unit_count; i-- > 0;) {
    for (size_t j = 0; j <= typed_count; ++j) {
      unsigned total = 0;
      if (units[i].kana) {
        if (j < typed_count && KanaKey(typed[j].value) == units[i].kana) total = at(i + 1, j + 1);
      } else {
        for (size_t end = j + 1; end <= typed_count && total < 2; ++end) total += at(i + 1, end);
      }
      at(i, j) = static_cast<uint8_t>((std::min)(total, 2u));
    }
  }
  if (at(0, 0) != 1) return {};
  std::vector<LivePiece> pieces;
  size_t j = 0;
  for (size_t i = 0; i < unit_count; ++i) {
    size_t end = j + 1;
    if (!units[i].kana) {
      while (at(i + 1, end) == 0) ++end;
    }
    LivePiece piece;
    piece.surface = units[i].text;
    for (size_t k = j; k < end; ++k) piece.reading += typed[k].text;
    // A run is cut only where a kana follows it: cutting right after a stem
    // ("書" of 書く) would show a word the answer did not give.
    piece.cut_after = units[i].kana.has_value();
    pieces.push_back(std::move(piece));
    j = end;
  }
  pieces.back().cut_after = true;
  return pieces;
}

}  // namespace

std::vector<LivePiece> BuildLivePieces(const std::string& reading, const std::string& surface,
                                       const std::vector<ipc::LiveSegment>& segments,
                                       bool auto_punctuation) {
  if (reading.empty() || surface.empty()) return {};
  bool segments_usable = !segments.empty();
  if (segments_usable) {
    std::string segment_reading;
    std::string segment_surface;
    for (const auto& segment : segments) {
      if (!segment.auto_punctuation) {
        if (segment.reading.empty()) segments_usable = false;
        segment_reading += segment.reading;
      }
      segment_surface += segment.surface;
    }
    segments_usable = segments_usable && segment_reading == reading && segment_surface == surface;
  }
  if (!segments_usable && auto_punctuation) return {};
  std::vector<ipc::LiveSegment> whole;
  if (!segments_usable) {
    ipc::LiveSegment segment;
    segment.reading = reading;
    segment.surface = surface;
    whole.push_back(std::move(segment));
  }
  std::vector<LivePiece> pieces;
  for (const auto& segment : segments_usable ? segments : whole) {
    if (segment.auto_punctuation) {
      LivePiece mark;
      mark.surface = segment.surface;
      mark.auto_punctuation = true;
      pieces.push_back(std::move(mark));
      continue;
    }
    auto aligned = AlignSegment(segment.reading, segment.surface);
    if (aligned.empty()) {
      LivePiece piece;
      piece.reading = segment.reading;
      piece.surface = segment.surface;
      piece.cut_after = true;
      pieces.push_back(std::move(piece));
      continue;
    }
    for (auto& piece : aligned) pieces.push_back(std::move(piece));
  }
  return pieces;
}

CarriedLiveText CarryLiveText(const std::vector<LivePiece>& pieces, std::string_view typed) {
  CarriedLiveText best;
  std::string kept;
  std::string marks;
  size_t consumed = 0;
  for (const auto& piece : pieces) {
    if (piece.auto_punctuation) {
      marks += piece.surface;
      continue;
    }
    if (piece.reading.empty() || typed.substr(consumed, piece.reading.size()) != piece.reading)
      break;
    kept += marks;
    marks.clear();
    kept += piece.surface;
    consumed += piece.reading.size();
    if (piece.cut_after) best = {kept, consumed};
  }
  return best;
}

}  // namespace azookey::tsf
