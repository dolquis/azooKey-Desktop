#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::learning {

// docs/typo-correction-learning-spec.md section 12.12.1. A typo pattern is
// stored only as abstraction tokens: one character plus the character before
// it, never the keys or the reading the user typed.
//
//   insertion(c, prev)   -> "<c>_insert_after_<prev>"
//   deletion(c, prev)    -> "<c>_delete_after_<prev>"
//   substitution(a -> b) -> "<a>_sub_<b>"
//   transposition(a, b)  -> "<a><b>_swap"
//
// Several edits are joined with '+'. prev is '^' at the start of the keys.
enum class TypoEditKind { Insertion, Deletion, Substitution, Transposition };

struct TypoEdit {
  TypoEditKind kind{TypoEditKind::Substitution};
  // Insertion / Deletion: the inserted or deleted character.
  // Substitution: the typed character. Transposition: the first character.
  char32_t first{};
  // Substitution: the intended character. Transposition: the second character.
  char32_t second{};
  // Insertion / Deletion: the character before it, or kTypoPatternStart.
  char32_t previous{};
};

inline constexpr char32_t kTypoPatternStart = U'^';

// The edit-distance cap of the accept filters (section 6), so a pattern never
// holds more edits than a learnable pair can differ by.
inline constexpr size_t kTypoPatternMaxEdits = 3;
// Upper bound on a stored pattern, in bytes. Three edits of the longest token
// (two 3-byte kana around "_insert_after_") stay well below it.
inline constexpr size_t kTypoPatternMaxBytes = 96;

// Section 12.12.1 / 12.13 alphabet: 'a'-'z', '-', '\'', hiragana U+3041-U+3096
// and the prolonged sound mark U+30FC.
bool IsTypoPatternCharacter(char32_t ch);

// Maps an edit list to its tokens. Returns nullopt, building nothing, when the
// list is empty, holds more than kTypoPatternMaxEdits edits, or any character
// lies outside the alphabet: such keys are discarded rather than abstracted.
std::optional<std::string> AbstractTypoEdits(const std::vector<TypoEdit>& edits);

// True when `pattern` is exactly a '+'-joined list of section 12.12.1 tokens
// over the alphabet, within kTypoPatternMaxEdits and kTypoPatternMaxBytes. The
// store refuses anything else, so raw keys or readings cannot be saved as a
// pattern.
bool IsValidTypoPattern(std::string_view pattern);

// Section 12.3.
bool IsKnownTypoType(std::string_view typo_type);

// Section 12.9 parameters. The defaults are the initial values; M52 calibrates
// them (section 12.16), keeping reject_weight > accept_weight.
struct TypoConfidenceParams {
  double base{0.0};
  double accept_weight{0.25};
  double reject_weight{0.45};
  double recency_half_life_days{60.0};
  double recency_amplitude{0.30};
  double app_bonus{0.20};
  double min_confidence{0.05};
  double max_confidence{0.95};
};

// Section 12.9: logit-space sum, sigmoid, clamp. A pattern never used
// (last_used_epoch_sec is nullopt) has no recency term; a future timestamp
// (clock change) counts as used now.
double ComputeTypoConfidence(uint32_t accept_count, uint32_t reject_count,
                             std::optional<uint64_t> last_used_epoch_sec, uint64_t now_epoch_sec,
                             bool app_bonus_applies, const TypoConfidenceParams& params = {});

}  // namespace azookey::learning
