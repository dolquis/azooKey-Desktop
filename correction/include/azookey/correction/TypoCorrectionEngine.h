#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/correction/KeyboardAdjacencyModel.h"
#include "azookey/correction/ReadingHypothesis.h"

namespace azookey::correction {

// docs/typo-correction-learning-spec.md section 12.16 LEN_MIN: a reading of
// this many code points or fewer is not corrected.
inline constexpr size_t kTypoMinReadingLength = 2;
// Section 6: readings and keys longer than this many code points are not corrected.
inline constexpr size_t kTypoMaxInputLength = 64;
// Edits per hypothesis, within the learning store's three.
inline constexpr size_t kTypoMaxEdits = 2;
// Corrections returned at most. The Host trims them to
// typoCorrection.maxReadingHypotheses after the dictionary constraint
// (section 12.4.4), because most readings this close to the input are not words.
inline constexpr size_t kMaxTypoHypotheses = 256;
// Section 12.9 base: a correction without a personal pattern has the
// confidence of a pattern with no history, below minConfidenceForRanking.
inline constexpr double kGeneralTypoConfidence = 0.5;

// Section 12.4.1 personalized_pattern_cost for one hypothesis.
struct PersonalizedPattern {
  double cost_reduction = 0.0;  // subtracted from the cost, which stays at or above 0
  double confidence = 0.0;      // the pattern's section 12.9 confidence, [0, 1]
  std::string pattern_id;
};

// Where personal patterns come from (the learning store, section 12.8). A
// result with a negative or non-finite reduction, or a confidence outside
// [0, 1], is ignored.
class PersonalizedPatternSource {
 public:
  virtual ~PersonalizedPatternSource() = default;
  virtual std::optional<PersonalizedPattern> Find(
      TypoType type, EditSpace space, const std::vector<CorrectionEdit>& edits) const = 0;
};

struct TypoCorrectionInput {
  std::string_view observed_reading;
  // Section 12.13 raw_keys: lowercase romaji keys, hiragana, or empty.
  std::string_view raw_keys;
};

struct TypoCorrectionOptions {
  KeyboardLayout keyboard_layout = KeyboardLayout::Auto;
  // Corrections at most; larger values act as kMaxTypoHypotheses.
  size_t limit = kMaxTypoHypotheses;
  const PersonalizedPatternSource* personalized = nullptr;
};

// Sections 12.1-12.4: the observed reading first (source Original), then up to
// `limit` correction hypotheses, cheapest first, then by reading. Nothing comes
// back for an empty reading; only the original comes back when the reading has
// kTypoMinReadingLength code points or fewer, either input is longer than
// kTypoMaxInputLength code points, is not UTF-8, or holds a character outside
// the section 12.12.1 alphabet.
//
// - Keys: when raw_keys is romaji and the built-in table converts it to the
//   observed reading, RomajiKanaConverter::FuzzyMatch proposes readings within
//   kTypoMaxEdits key edits. A reading that AmbiguousReadings gives raw_keys
//   is a reading choice, not a correction (docs/legacy-parity-spec.md
//   section 5.4), and is left out.
// - Reading: single and double substitutions of the observed reading's kana
//   by its voiced / semi-voiced or small / similar-shaped counterparts.
//
// Each reading appears once at its lowest cost. A hypothesis whose reading
// differs from the observed one only by such kana substitutions takes
// dakuten_confusion or kana_shape_confusion; otherwise each key edit is
// classified (section 12.3) and the hypothesis takes the type that comes first
// in the section 12.3 table. Key edits that fit no type drop the hypothesis.
std::vector<ReadingHypothesis> GenerateReadingHypotheses(const TypoCorrectionInput& input,
                                                         const TypoCorrectionOptions& options = {});

}  // namespace azookey::correction
