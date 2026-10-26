#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::core {

// M63 (docs/mixed-english-auto-conversion-spec.md section 6): splits the raw
// romaji of a composition into Japanese (J) and English (E) segments.

enum class MixedRomajiSegmentKind : uint8_t {
  Japanese,
  English,
};

// Section 8.1, before any learning; the Host lowers it from the English
// learning channel (M63-B).
enum class EnglishSegmentConfidence : uint8_t {
  Low,
  High,
};

// One English dictionary surface for a lowercased key (M60 section 4.4).
struct EnglishWordMatch {
  std::string surface;
  uint8_t flags{};
};

// Returns the dictionary surfaces for lower_key, most frequent first; empty
// when there are none. An empty function means no dictionary (section 6.1).
using EnglishWordLookup = std::function<std::vector<EnglishWordMatch>(std::string_view lower_key)>;

struct MixedRomajiSegment {
  MixedRomajiSegmentKind kind{MixedRomajiSegmentKind::Japanese};
  // Byte range in the raw romaji.
  size_t raw_begin{};
  size_t raw_end{};
  std::string kana;     // J: the built-in romaji table's kana of the range.
  std::string surface;  // E: section 6.4.
  EnglishSegmentConfidence confidence{EnglishSegmentConfidence::Low};  // E only.
};

struct MixedRomajiSplit {
  std::vector<MixedRomajiSegment> segments;
};

struct MixedRomajiSplitOptions {
  size_t max_raw_length{64};  // Section 10.
  size_t max_splits{8};       // Section 6.1.
  // M60 section 4.2 (inlineEnglishMinLength): a shorter range never becomes E.
  size_t min_english_length{2};
};

// The splits, best first. Empty when the raw romaji is empty, longer than
// max_raw_length or not printable ASCII; the caller then handles the request
// as with M63 off (section 10).
// M63-A judges only the whole composition (section 8.2): it is one E segment
// when it matches the dictionary or starts with a Shift capital, and one J
// segment otherwise. M63-B adds the mixed lattice (sections 6.2 and 6.3).
std::vector<MixedRomajiSplit> SplitMixedRomaji(std::string_view raw_romaji,
                                               const EnglishWordLookup& lookup,
                                               const MixedRomajiSplitOptions& options = {});

}  // namespace azookey::core
