#include "azookey/host/TypoCorrectionPolicy.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

#include "azookey/learning/TypoPattern.h"

namespace azookey::host {

namespace {

learning::TypoEditKind ToTypoEditKind(correction::CorrectionEditKind kind) {
  switch (kind) {
    case correction::CorrectionEditKind::Insertion:
      return learning::TypoEditKind::Insertion;
    case correction::CorrectionEditKind::Deletion:
      return learning::TypoEditKind::Deletion;
    case correction::CorrectionEditKind::Transposition:
      return learning::TypoEditKind::Transposition;
    case correction::CorrectionEditKind::Substitution:
      break;
  }
  return learning::TypoEditKind::Substitution;
}

std::optional<std::string> AbstractEdits(const std::vector<correction::CorrectionEdit>& edits) {
  std::vector<learning::TypoEdit> typo_edits;
  typo_edits.reserve(edits.size());
  for (const auto& edit : edits)
    typo_edits.push_back({ToTypoEditKind(edit.kind), edit.first, edit.second, edit.previous});
  return learning::AbstractTypoEdits(typo_edits);
}

std::string PatternKey(std::string_view typo_type, std::string_view observed,
                       std::string_view intended) {
  // Neither a typo_type nor a section 12.12.1 pattern holds a tab.
  std::string key(typo_type);
  key.append(1, '\t').append(observed).append(1, '\t').append(intended);
  return key;
}

uint32_t SaturatingAdd(uint32_t a, uint32_t b) {
  return b > (std::numeric_limits<uint32_t>::max)() - a ? (std::numeric_limits<uint32_t>::max)()
                                                        : a + b;
}

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

double TypoDictionaryHitScore(double word_score) {
  if (!std::isfinite(word_score)) return 0.0;
  return std::clamp(word_score / kTypoDictionaryFullScore, 0.0, 1.0);
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
    if (!IsStrongTypoPattern(word.evidence, guards))
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

std::optional<TypoPatternPair> AbstractCorrectionEdits(
    const std::vector<correction::CorrectionEdit>& edits) {
  auto observed = AbstractEdits(edits);
  auto intended = AbstractEdits(correction::InvertEdits(edits));
  if (!observed || !intended) return std::nullopt;
  return TypoPatternPair{std::move(*observed), std::move(*intended)};
}

TypoPatternIndex::TypoPatternIndex(const std::vector<learning::TypoPatternRecord>& patterns) {
  for (const auto& pattern : patterns) {
    auto key = PatternKey(pattern.typo_type, pattern.observed_pattern, pattern.intended_pattern);
    auto [at, added] = by_key_.try_emplace(key, pattern);
    auto& row = at->second;
    if (!added) {
      row.accept_count = SaturatingAdd(row.accept_count, pattern.accept_count);
      row.reject_count = SaturatingAdd(row.reject_count, pattern.reject_count);
      if (pattern.last_used_at && (!row.last_used_at || *pattern.last_used_at > *row.last_used_at))
        row.last_used_at = pattern.last_used_at;
      if (pattern.id < row.id) {
        key_by_id_.erase(std::to_string(row.id));
        row.id = pattern.id;
      }
    }
    key_by_id_.insert_or_assign(std::to_string(row.id), std::move(key));
    max_accept_count_ = (std::max)(max_accept_count_, row.accept_count);
  }
}

const learning::TypoPatternRecord* TypoPatternIndex::Find(std::string_view typo_type,
                                                          const TypoPatternPair& patterns) const {
  const auto at = by_key_.find(PatternKey(typo_type, patterns.observed, patterns.intended));
  return at == by_key_.end() ? nullptr : &at->second;
}

const learning::TypoPatternRecord* TypoPatternIndex::FindById(std::string_view pattern_id) const {
  const auto key = key_by_id_.find(pattern_id);
  if (key == key_by_id_.end()) return nullptr;
  const auto at = by_key_.find(key->second);
  return at == by_key_.end() ? nullptr : &at->second;
}

TypoPatternLookup::TypoPatternLookup(const TypoPatternIndex& index, uint64_t now_epoch_sec,
                                     const TypoGuards& guards)
    : index_(index), now_epoch_sec_(now_epoch_sec), guards_(guards) {}

std::optional<correction::PersonalizedPattern> TypoPatternLookup::Find(
    correction::TypoType type, correction::EditSpace /*space*/,
    const std::vector<correction::CorrectionEdit>& edits) const {
  // The key and the reading alphabets of section 12.12.1 do not meet, so the patterns tell the
  // two spaces apart.
  const auto patterns = AbstractCorrectionEdits(edits);
  if (!patterns) return std::nullopt;
  const auto* row = index_.Find(correction::TypoTypeName(type), *patterns);
  if (!row) return std::nullopt;
  correction::PersonalizedPattern found;
  found.confidence = learning::ComputeTypoConfidence(row->accept_count, row->reject_count,
                                                     row->last_used_at, now_epoch_sec_, false);
  found.pattern_id = std::to_string(row->id);
  TypoHypothesisEvidence evidence;
  evidence.confidence = found.confidence;
  evidence.pattern = TypoPatternHistory{row->accept_count, row->reject_count};
  if (IsStrongTypoPattern(evidence, guards_))
    found.cost_reduction = kTypoPersonalCostReduction * found.confidence;
  return found;
}

std::optional<TypoPatternHistory> TypoPatternLookup::History(std::string_view pattern_id) const {
  const auto* row = index_.FindById(pattern_id);
  if (!row) return std::nullopt;
  return TypoPatternHistory{row->accept_count, row->reject_count};
}

}  // namespace azookey::host
