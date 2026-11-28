#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/core/Candidate.h"
#include "azookey/correction/TypoCorrectionEngine.h"
#include "azookey/learning/TypoLearningStore.h"

namespace azookey::host {

// docs/typo-correction-learning-spec.md section 12.6: the typo correction modes.
enum class TypoCorrectionMode { Off, Suggest, Rank, Aggressive };

// Reads EngineConfig::typo_correction_mode. The v1 auto_replace is aggressive (section 12.6); any
// other value converts as suggest, as the v1 engine did.
TypoCorrectionMode ParseTypoCorrectionMode(std::string_view mode);

// Section 12.16 initial values of the section 12.5, 12.10 and 12.11 constants.
inline constexpr double kTypoWeakScore = 0.40;             // S_WEAK
inline constexpr double kTypoSmallGap = 0.15;              // G_GAP
inline constexpr double kTypoRejectPenalty = 0.50;         // P_reject
inline constexpr double kTypoStrongTop1Penalty = 0.40;     // P_strong_top1
inline constexpr double kTypoStrongTop1 = 0.85;            // S_STRONG
inline constexpr double kTypoLowConfidencePenalty = 0.50;  // P_lowconf

// Section 12.5 for the reading as typed. strong_pat is a hypothesis's own (IsStrongTypoPattern).
struct TypoActivationInput {
  // r1 and r2: the scores of its first and second candidates, nullopt when there are none.
  std::optional<double> top1_score;
  std::optional<double> top2_score;
  // dict_best in [0, 1]; 0 without a dictionary hit.
  double dictionary_best{0.0};
};
// weak, small_gap or no_dict.
bool TypoCorrectionActivates(const TypoActivationInput& input);

// A personal pattern's history (section 12.8 typo_patterns).
struct TypoPatternHistory {
  uint32_t accept_count{};
  uint32_t reject_count{};
};

struct TypoGuards {
  uint32_t min_count{3};  // typo_min_count, the warm-up of section 12.11
  double min_confidence_for_ranking{0.70};
  double min_confidence_for_top_candidate{0.90};
};

// What sections 12.10 and 12.11 read of one hypothesis.
struct TypoHypothesisEvidence {
  double confidence{0.5};  // typo_confidence
  std::optional<TypoPatternHistory> pattern;
  double reading_similarity{0.0};
  double dictionary_hit_score{1.0};
};

// Section 12.5 dict_best and section 12.10 s1_strength of a word of the typed reading whose
// dictionary score (learning::DictionaryStore) is `word_score`: the score over that of a Sudachi
// word of the highest frequency, in [0, 1].
inline constexpr double kTypoDictionaryFullScore = 1.4;
double TypoDictionaryHitScore(double word_score);

// reading_similarity_score: 1 - edit_distance / the longer reading's code points.
double TypoReadingSimilarity(size_t edit_distance, size_t observed_codepoints,
                             size_t corrected_codepoints);
// strong_pat: a pattern past the warm-up whose confidence reaches minConfidenceForRanking.
bool IsStrongTypoPattern(const TypoHypothesisEvidence& evidence, const TypoGuards& guards);
// Section 12.10 typo_score, with context_score and app_factor_typo at 1.0. `top1_strength` is
// s1_strength.
double TypoScore(const TypoHypothesisEvidence& evidence, double top1_strength,
                 const TypoGuards& guards);
// Section 12.11 under aggressive: every guard passes, so the hypothesis may become the first
// candidate.
bool TypoHypothesisMayLead(const TypoHypothesisEvidence& evidence, double top1_strength,
                           const TypoGuards& guards);

struct TypoHypothesisWord {
  core::Candidate candidate;  // the word, with the hypothesis as its reading
  TypoHypothesisEvidence evidence;
};

// Sections 12.7 and 12.11 for rank and aggressive. Puts at most `max_words` of `words`, best
// typo_score first and the given order on a tie, right after the first candidate of
// `candidates`. A word of a strong pattern (IsStrongTypoPattern) shows as a normal candidate;
// the others are labelled "もしかして: <reading>". Under aggressive the best word that may lead
// goes first instead, unlabelled. A surface already in `candidates` is skipped. A placed word
// takes the score of the candidate it follows (the lead, that of the one it displaces), so a list
// in score order stays in score order.
void PlaceTypoHypothesisWords(std::vector<core::Candidate>& candidates,
                              std::vector<TypoHypothesisWord> words, TypoCorrectionMode mode,
                              double top1_strength, const TypoGuards& guards, size_t max_words);

// The section 12.12.1 observed_pattern and intended_pattern of a hypothesis's edits; nullopt when
// learning::AbstractTypoEdits refuses them.
struct TypoPatternPair {
  std::string observed;
  std::string intended;
};
std::optional<TypoPatternPair> AbstractCorrectionEdits(
    const std::vector<correction::CorrectionEdit>& edits);

// DEV-2149 decision 7 interim section 12.4.1 personalized_pattern_cost: a strong pattern takes
// this times its confidence off a hypothesis's cost.
inline constexpr double kTypoPersonalCostReduction = 0.5;

// The patterns of a typo store (TypoLearningStore::Patterns()), by typo_type and the two section
// 12.12.1 patterns. Rows that differ only in keyboard_layout (NULL or auto, the one layout) count
// together under the lowest id, their counts added and the later last_used_at kept.
class TypoPatternIndex {
 public:
  explicit TypoPatternIndex(const std::vector<learning::TypoPatternRecord>& patterns);
  const learning::TypoPatternRecord* Find(std::string_view typo_type,
                                          const TypoPatternPair& patterns) const;
  // `pattern_id` as correction::ReadingHypothesis::pattern_id holds it (the id in decimal).
  const learning::TypoPatternRecord* FindById(std::string_view pattern_id) const;
  // No pattern is past the warm-up while this is below typo_min_count.
  uint32_t max_accept_count() const { return max_accept_count_; }

 private:
  std::map<std::string, learning::TypoPatternRecord, std::less<>> by_key_;
  std::map<std::string, std::string, std::less<>> key_by_id_;
  uint32_t max_accept_count_{0};
};

// The personalized patterns of one query: section 12.9 confidence at `now_epoch_sec` without the
// app bonus. Every pattern that matches is returned, a young or rejected one with no cost
// reduction, so its history still reaches sections 12.10 and 12.11.
class TypoPatternLookup final : public correction::PersonalizedPatternSource {
 public:
  TypoPatternLookup(const TypoPatternIndex& index, uint64_t now_epoch_sec,
                    const TypoGuards& guards);
  std::optional<correction::PersonalizedPattern> Find(
      correction::TypoType type, correction::EditSpace space,
      const std::vector<correction::CorrectionEdit>& edits) const override;
  std::optional<TypoPatternHistory> History(std::string_view pattern_id) const;

 private:
  const TypoPatternIndex& index_;
  uint64_t now_epoch_sec_;
  TypoGuards guards_;
};

}  // namespace azookey::host
