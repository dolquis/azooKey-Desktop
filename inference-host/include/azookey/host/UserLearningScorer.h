#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "azookey/core/Candidate.h"
#include "azookey/learning/LearningStore.h"

namespace azookey::host {

// How recency is computed. Legacy is the M7 exp(-0.15 * days) and stays the
// production default until M52 calibrates the section 5 half lives, so the
// benchmark baseline does not move (user-learning-enhancement-spec section 14.4).
enum class LearningDecayMode : uint8_t { Legacy, CategoryHalfLife };

// user-learning-enhancement-spec section 13 defaults.
struct UserLearningScorerConfig {
  LearningDecayMode decay_mode{LearningDecayMode::Legacy};
  bool app_profile_enabled{true};
  double same_app_weight{1.2};
  double different_app_weight{0.8};
  double reject_penalty_step{0.3};
  double min_correction_penalty{0.0};
};

struct UserLearningContext {
  std::string reading;
  // Foreground process name; normalized with NormalizeLearningAppName. Empty
  // when unknown.
  std::string app_name;
  uint64_t now_epoch_sec{};
};

// M53 category_mask of a (reading, surface) pair; 0 when unknown.
using CategoryLookup =
    std::function<uint16_t(const std::string& reading, const std::string& surface)>;

// user-learning-enhancement-spec section 7:
// user_score = sum over the pair's app rows of
//   log(1 + commit_count) * recency * app_profile_weight * correction_penalty.
class UserLearningScorer {
 public:
  UserLearningScorer(const learning::LearningStore* store, CategoryLookup category_lookup,
                     UserLearningScorerConfig config = {});

  // Adds user_score to every candidate and stable-sorts by score, highest
  // first. A candidate whose score is not finite sorts last.
  void Score(const UserLearningContext& context, std::span<core::Candidate> candidates) const;

  // 0 when the pair has no row or scoring fails (section 7: continue with 0).
  double CalcUserScore(const UserLearningContext& context, const std::string& surface) const;
  double RecencyScore(const learning::LearningRecord& record, uint16_t category_mask,
                      uint64_t now_epoch_sec) const;
  double AppProfileWeight(std::string_view record_app, std::string_view current_app) const;
  double CorrectionPenalty(const learning::LearningRecord& record) const;

 private:
  const learning::LearningStore* store_;
  CategoryLookup category_lookup_;
  UserLearningScorerConfig config_;
};

}  // namespace azookey::host
