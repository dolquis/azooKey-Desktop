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
  // Section 6.3: the sum of the edge costs; lower is better. 0 for the
  // whole-composition judgement of M63-A.
  double cost{};
};

struct MixedRomajiSplitOptions {
  size_t max_raw_length{64};  // Section 10.
  size_t max_splits{8};       // Section 6.1.
  // M60 section 4.2 (inlineEnglishMinLength): a shorter range never becomes E.
  size_t min_english_length{2};
  // M63-B (sections 6.2 and 6.3): split a composition that is not one word
  // with the mixed lattice. Off keeps the M63-A whole-composition judgement
  // only.
  bool split_inline{false};
  // Section 6.2: the shortest E edge inside a composition, and the shortest
  // one that starts with a Shift capital.
  size_t min_inline_english_length{3};
  size_t min_shift_inline_english_length{2};
  // Bounds the dictionary lookups per composition.
  size_t max_inline_english_length{24};
  // Section 15.2: false treats every E segment of the lattice as low
  // confidence, so no in-sentence word turns English on its own.
  bool inline_high_confidence{true};
};

// The splits, best first. Empty when the raw romaji is empty, longer than
// max_raw_length or not printable ASCII; the caller then handles the request
// as with M63 off (section 10).
// The whole composition is one E segment when it matches the dictionary
// (section 8.2, without the section 6.3 penalty). Otherwise, with
// split_inline and a dictionary, the mixed lattice gives up to max_splits
// splits (sections 6.2 and 6.3); a composition the lattice leaves all J is
// still one E segment when it starts with a Shift capital (section 6.1), as is
// one without a dictionary. Anything else is one J segment.
std::vector<MixedRomajiSplit> SplitMixedRomaji(std::string_view raw_romaji,
                                               const EnglishWordLookup& lookup,
                                               const MixedRomajiSplitOptions& options = {});

}  // namespace azookey::core
