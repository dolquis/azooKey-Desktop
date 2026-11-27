#include "azookey/host/TypoCorrectionPolicy.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <utility>

namespace azookey::host {

namespace {

// Section 12.10 net_reject.
uint32_t NetReject(const TypoHypothesisEvidence& evidence) {
  if (!evidence.pattern) return 0;
  const auto& pattern = *evidence.pattern;
  return pattern.reject_count > pattern.accept_count ? pattern.reject_count - pattern.accept_count
                                                     : 0;
}

}  // namespace

TypoCorrectionMode ParseTypoCorrectionMode(std::string_view mode) {
  if (mode == "off") return TypoCorrectionMode::Off;
  if (mode == "rank") return TypoCorrectionMode::Rank;
  if (mode == "aggressive" || mode == "auto_replace") return TypoCorrectionMode::Aggressive;
  return TypoCorrectionMode::Suggest;
}

bool TypoCorrectionActivates(const TypoActivationInput& input) {
  // No candidate, or r1 <= 0: weak, without dividing by r1.
  if (!input.top1_score || !std::isfinite(*input.top1_score) || *input.top1_score <= 0.0)
    return true;
  const double r2 = input.top2_score && std::isfinite(*input.top2_score) ? *input.top2_score : 0.0;
  const double s2 = std::clamp(r2 / *input.top1_score, 0.0, 1.0);
  const bool weak = input.dictionary_best < kTypoWeakScore;
  const bool small_gap = (1.0 - s2) < kTypoSmallGap;
  const bool no_dict = input.dictionary_best == 0.0;
  return weak || small_gap || no_dict;
}

double TypoReadingSimilarity(size_t edit_distance, size_t observed_codepoints,
                             size_t corrected_codepoints) {
  const size_t longer = (std::max)(observed_codepoints, corrected_codepoints);
  if (longer == 0) return 0.0;
  return std::clamp(1.0 - static_cast<double>(edit_distance) / static_cast<double>(longer), 0.0,
                    1.0);
}

bool IsStrongTypoPattern(const TypoHypothesisEvidence& evidence, const TypoGuards& guards) {
  return evidence.pattern && evidence.pattern->accept_count >= guards.min_count &&
         evidence.confidence >= guards.min_confidence_for_ranking;
}

double TypoScore(const TypoHypothesisEvidence& evidence, double top1_strength,
                 const TypoGuards& guards) {
  double penalty = NetReject(evidence) >= 1 ? kTypoRejectPenalty : 0.0;
  penalty += kTypoStrongTop1Penalty * (std::max)(0.0, top1_strength - kTypoStrongTop1);
  penalty += kTypoLowConfidencePenalty *
             (std::max)(0.0, guards.min_confidence_for_ranking - evidence.confidence);
  const double score =
      evidence.confidence * evidence.dictionary_hit_score * evidence.reading_similarity - penalty;
  return std::clamp(score, 0.0, 1.0);
}

bool TypoHypothesisMayLead(const TypoHypothesisEvidence& evidence, double top1_strength,
                           const TypoGuards& guards) {
  return IsStrongTypoPattern(evidence, guards) &&
         evidence.confidence >= guards.min_confidence_for_top_candidate &&
         NetReject(evidence) == 0 && top1_strength < kTypoStrongTop1;
}

void PlaceTypoHypothesisWords(std::vector<core::Candidate>& candidates,
                              std::vector<TypoHypothesisWord> words, TypoCorrectionMode mode,
                              double top1_strength, const TypoGuards& guards, size_t max_words) {
  if (max_words == 0 || words.empty() ||
      (mode != TypoCorrectionMode::Rank && mode != TypoCorrectionMode::Aggressive))
    return;
  std::vector<std::pair<double, size_t>> order;
  order.reserve(words.size());
  for (size_t i = 0; i < words.size(); ++i)
    order.emplace_back(TypoScore(words[i].evidence, top1_strength, guards), i);
  std::stable_sort(order.begin(), order.end(),
                   [](const auto& a, const auto& b) { return a.first > b.first; });

  std::optional<core::Candidate> lead;
  std::vector<core::Candidate> frame;
  const auto shown = [&](const std::string& surface) {
    const auto same = [&](const core::Candidate& c) { return c.surface == surface; };
    return std::any_of(candidates.begin(), candidates.end(), same) ||
           std::any_of(frame.begin(), frame.end(), same) || (lead && same(*lead));
  };
  for (const auto& [score, index] : order) {
    if (frame.size() + (lead ? 1 : 0) == max_words) break;
    auto& word = words[index];
    if (shown(word.candidate.surface)) continue;
    if (mode == TypoCorrectionMode::Aggressive && !lead &&
        TypoHypothesisMayLead(word.evidence, top1_strength, guards)) {
      lead = std::move(word.candidate);
      continue;
    }
    word.candidate.description = "もしかして: " + word.candidate.reading;
    frame.push_back(std::move(word.candidate));
  }

  if (lead) {
    if (!candidates.empty()) lead->score = candidates.front().score;
    candidates.insert(candidates.begin(), std::move(*lead));
  }
  // After the first candidate of the conversion, which a lead moved to the second place.
  const size_t at = (std::min)(candidates.size(), static_cast<size_t>(lead ? 2 : 1));
  if (at > 0)
    for (auto& word : frame) word.score = candidates[at - 1].score;
  candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(at),
                    std::make_move_iterator(frame.begin()), std::make_move_iterator(frame.end()));
}

}  // namespace azookey::host
