#pragma once

#include <cstdint>

namespace azookey::learning {

// user-learning-enhancement-spec section 5. Initial values, calibrated by M52;
// the order general < proper noun / typo < technical, temporary shortest, is
// an invariant.
inline constexpr double kGeneralHalfLifeDays = 30.0;
inline constexpr double kProperNounHalfLifeDays = 90.0;
inline constexpr double kTechnicalHalfLifeDays = 120.0;
inline constexpr double kTemporaryTopicHalfLifeDays = 14.0;
inline constexpr double kTypoPatternHalfLifeDays = 60.0;
// The M7 decay, exp(-0.15 * days), kept as the production default until M52
// calibrates the half lives (section 14.4).
inline constexpr double kLegacyDecayPerDay = 0.15;

// Half life for an M53 category_mask (auto-word-registration-spec section
// 14.4, bit order of its table). The longest applicable half life wins.
double HalfLifeDaysForCategoryMask(uint16_t category_mask);

// exp(-ln2 * days / half_life_days); a clock rollback counts as zero days.
double HalfLifeRecency(uint64_t last_updated_epoch_sec, uint64_t now_epoch_sec,
                       double half_life_days);

// exp(-0.15 * days); a clock rollback counts as zero days.
double LegacyRecency(uint64_t last_updated_epoch_sec, uint64_t now_epoch_sec);

}  // namespace azookey::learning
