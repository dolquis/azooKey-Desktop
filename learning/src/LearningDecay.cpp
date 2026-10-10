#include "azookey/learning/LearningDecay.h"

#include <cmath>
#include <numbers>

#include "azookey/learning/DictionaryCategory.h"

namespace azookey::learning {

namespace {
constexpr uint16_t kProperNounBits =
    CategoryBit(DictionaryCategory::PersonName) | CategoryBit(DictionaryCategory::PlaceName) |
    CategoryBit(DictionaryCategory::StationName) | CategoryBit(DictionaryCategory::ProductName) |
    CategoryBit(DictionaryCategory::Software) | CategoryBit(DictionaryCategory::AnimeGame) |
    CategoryBit(DictionaryCategory::CompanyOrg);

double ElapsedDays(uint64_t last_updated_epoch_sec, uint64_t now_epoch_sec) {
  const double elapsed_seconds = now_epoch_sec >= last_updated_epoch_sec
                                     ? static_cast<double>(now_epoch_sec - last_updated_epoch_sec)
                                     : 0.0;
  return elapsed_seconds / (60.0 * 60.0 * 24.0);
}
}  // namespace

double HalfLifeDaysForCategoryMask(uint16_t category_mask) {
  if (category_mask & CategoryBit(DictionaryCategory::Technical)) return kTechnicalHalfLifeDays;
  if (category_mask & kProperNounBits) return kProperNounHalfLifeDays;
  // A neologism fades like a typing habit: longer than general words, shorter
  // than proper nouns. This matches the M53 obsolete_penalty table.
  if (category_mask & CategoryBit(DictionaryCategory::Neologism)) return kTypoPatternHalfLifeDays;
  return kGeneralHalfLifeDays;
}

double HalfLifeRecency(uint64_t last_updated_epoch_sec, uint64_t now_epoch_sec,
                       double half_life_days) {
  if (!(half_life_days > 0.0)) return 1.0;
  return std::exp(-std::numbers::ln2 * ElapsedDays(last_updated_epoch_sec, now_epoch_sec) /
                  half_life_days);
}

double LegacyRecency(uint64_t last_updated_epoch_sec, uint64_t now_epoch_sec) {
  return std::exp(-kLegacyDecayPerDay * ElapsedDays(last_updated_epoch_sec, now_epoch_sec));
}

}  // namespace azookey::learning
