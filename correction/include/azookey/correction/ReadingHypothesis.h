#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::correction {

// docs/typo-correction-learning-spec.md section 12.3, in the table's order.
enum class TypoType {
  AdjacentKey,
  MissingChar,
  ExtraChar,
  Transposition,
  DoubleKey,
  RomajiVariant,
  NHandling,
  SmallTsu,
  LongVowel,
  DakutenConfusion,
  KanaShapeConfusion,
};

// The section 12.3 name, such as "adjacent_key".
std::string_view TypoTypeName(TypoType type);

// One edit of what the user typed against what they meant, in the direction of
// section 12.12.1 align(observed, intended). The fields match
// learning::TypoEdit, so the learning store can turn a list into a pattern.
enum class CorrectionEditKind { Insertion, Deletion, Substitution, Transposition };

inline constexpr char32_t kEditStart = U'^';

struct CorrectionEdit {
  CorrectionEditKind kind{CorrectionEditKind::Substitution};
  // Insertion: the extra character typed. Deletion: the character left out.
  // Substitution: the typed character. Transposition: the first typed character.
  char32_t first{};
  // Substitution: the intended character. Transposition: the second typed
  // character (the user typed first, second for second, first).
  char32_t second{};
  // The typed character before the edit, or kEditStart at the beginning.
  char32_t previous{};

  bool operator==(const CorrectionEdit&) const = default;
};

// Where the edits were taken: the typed keys (raw_keys) or the reading.
enum class EditSpace { Keys, Reading };

// The correction direction of `edits`: insertion and deletion trade places,
// a substitution of b for a becomes one of a for b, and a swap of ab becomes
// one of ba. Section 12.12.1 builds intended_pattern from this list.
std::vector<CorrectionEdit> InvertEdits(const std::vector<CorrectionEdit>& edits);

enum class HypothesisSource { Original, TypoCorrection };

// Section 12.2.
struct ReadingHypothesis {
  std::string reading;
  HypothesisSource source{HypothesisSource::Original};
  double confidence{1.0};
  std::optional<TypoType> typo_type;  // none for the original reading
  std::string pattern_id;             // set when a personalized pattern applies
  // Code points between the observed reading and `reading` (plain Levenshtein).
  size_t edit_distance{0};
  bool personalized{false};
  // Section 12.4.1 weighted edit cost after the personalized reduction; 0 for
  // the original reading.
  double cost{0.0};
  // The edits in the observed direction; empty for the original reading.
  std::vector<CorrectionEdit> edits;
  EditSpace edit_space{EditSpace::Keys};
};

}  // namespace azookey::correction
