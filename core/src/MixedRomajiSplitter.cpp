#include "azookey/core/MixedRomajiSplitter.h"

#include <algorithm>

#include "azookey/core/RomajiKanaConverter.h"

namespace azookey::core {

namespace {

bool IsAsciiUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool IsAsciiLower(char c) { return c >= 'a' && c <= 'z'; }
bool IsPrintableAscii(char c) { return c > ' ' && c <= '~'; }

std::string Lower(std::string_view raw) {
  std::string lower(raw);
  for (auto& c : lower)
    if (IsAsciiUpper(c)) c = static_cast<char>(c - 'A' + 'a');
  return lower;
}

}  // namespace

std::vector<MixedRomajiSplit> SplitMixedRomaji(std::string_view raw_romaji,
                                               const EnglishWordLookup& lookup,
                                               const MixedRomajiSplitOptions& options) {
  if (raw_romaji.empty() || raw_romaji.size() > options.max_raw_length || options.max_splits == 0 ||
      !std::all_of(raw_romaji.begin(), raw_romaji.end(), IsPrintableAscii))
    return {};
  const auto lower = Lower(raw_romaji);
  const auto kana = RomajiKanaConverter::ConvertForCommit(lower);

  MixedRomajiSegment segment;
  segment.raw_end = raw_romaji.size();
  const bool has_letter = std::any_of(lower.begin(), lower.end(), IsAsciiLower);
  if (has_letter && raw_romaji.size() >= options.min_english_length) {
    const auto matches = lookup ? lookup(lower) : std::vector<EnglishWordMatch>{};
    const bool shift_first = IsAsciiUpper(raw_romaji.front());
    if (!matches.empty() || shift_first) {
      segment.kind = MixedRomajiSegmentKind::English;
      // Section 6.4: typed capitals keep the keystrokes; otherwise the most
      // frequent dictionary surface.
      const bool has_upper = std::any_of(raw_romaji.begin(), raw_romaji.end(), IsAsciiUpper);
      segment.surface = has_upper ? std::string(raw_romaji) : matches.front().surface;
      // Section 8.1: (i) a Shift capital first, or (ii) letters left after the
      // romaji-to-kana conversion together with a dictionary match.
      const bool nonkana = std::any_of(kana.begin(), kana.end(), IsAsciiLower);
      segment.confidence = shift_first || (nonkana && !matches.empty())
                               ? EnglishSegmentConfidence::High
                               : EnglishSegmentConfidence::Low;
    }
  }
  if (segment.kind == MixedRomajiSegmentKind::Japanese) segment.kana = kana;
  return {MixedRomajiSplit{{std::move(segment)}}};
}

}  // namespace azookey::core
