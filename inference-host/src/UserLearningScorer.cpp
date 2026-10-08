#include "azookey/host/UserLearningScorer.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

#include "azookey/learning/LearningDecay.h"

namespace azookey::host {

namespace {
bool IsTypoEvent(learning::LearningEventType event) {
  return event == learning::LearningEventType::TypoAccept ||
         event == learning::LearningEventType::TypoReject;
}

double SortKey(const core::Candidate& candidate) {
  return std::isfinite(candidate.score) ? candidate.score
                                        : -std::numeric_limits<double>::infinity();
}
}  // namespace

UserLearningScorer::UserLearningScorer(const learning::LearningStore* store,
                                       CategoryLookup category_lookup,
                                       UserLearningScorerConfig config)
    : store_(store), category_lookup_(std::move(category_lookup)), config_(config) {}

void UserLearningScorer::Score(const UserLearningContext& context,
                               std::span<core::Candidate> candidates) const {
  for (auto& candidate : candidates) {
    candidate.score += CalcUserScore(context, candidate.surface);
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const auto& lhs, const auto& rhs) { return SortKey(lhs) > SortKey(rhs); });
}

double UserLearningScorer::CalcUserScore(const UserLearningContext& context,
                                         const std::string& surface) const {
  if (!store_) return 0.0;
  try {
    const auto* rows = store_->Rows(context.reading, surface);
    if (!rows) return 0.0;
    const uint16_t category_mask =
        config_.decay_mode == LearningDecayMode::CategoryHalfLife && category_lookup_
            ? category_lookup_(context.reading, surface)
            : 0;
    const auto current_app = learning::NormalizeLearningAppName(context.app_name);
    double total = 0.0;
    for (const auto& [app_name, record] : *rows) {
      total += std::log1p(static_cast<double>(record.commit_count)) *
               RecencyScore(record, category_mask, context.now_epoch_sec) *
               AppProfileWeight(app_name, current_app) * CorrectionPenalty(record);
    }
    return std::isfinite(total) ? total : 0.0;
  } catch (const std::exception&) {
    return 0.0;
  }
}

double UserLearningScorer::RecencyScore(const learning::LearningRecord& record,
                                        uint16_t category_mask, uint64_t now_epoch_sec) const {
  if (config_.decay_mode == LearningDecayMode::Legacy) {
    return learning::LegacyRecency(record.last_updated_epoch_sec, now_epoch_sec);
  }
  double half_life = learning::HalfLifeDaysForCategoryMask(category_mask);
  // A typo row is a typing habit (section 5); the longest half life applies.
  if (IsTypoEvent(record.last_event)) {
    half_life = std::max(half_life, learning::kTypoPatternHalfLifeDays);
  }
  return learning::HalfLifeRecency(record.last_updated_epoch_sec, now_epoch_sec, half_life);
}

double UserLearningScorer::AppProfileWeight(std::string_view record_app,
                                            std::string_view current_app) const {
  if (!config_.app_profile_enabled || record_app.empty() || current_app.empty()) return 1.0;
  return record_app == current_app ? config_.same_app_weight : config_.different_app_weight;
}

double UserLearningScorer::CorrectionPenalty(const learning::LearningRecord& record) const {
  const uint64_t net_reject =
      record.reject_count > record.accept_count ? record.reject_count - record.accept_count : 0;
  return std::max(config_.min_correction_penalty,
                  1.0 - config_.reject_penalty_step * static_cast<double>(net_reject));
}

}  // namespace azookey::host
