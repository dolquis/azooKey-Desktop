#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "azookey/core/Candidate.h"

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
// `candidates`, each labelled "もしかして: <reading>". Under aggressive the best word that may
// lead goes first instead, unlabelled. A surface already in `candidates` is skipped. A placed
// word takes the score of the candidate it follows (the lead, that of the one it displaces), so a
// list in score order stays in score order.
void PlaceTypoHypothesisWords(std::vector<core::Candidate>& candidates,
                              std::vector<TypoHypothesisWord> words, TypoCorrectionMode mode,
                              double top1_strength, const TypoGuards& guards, size_t max_words);

}  // namespace azookey::host
