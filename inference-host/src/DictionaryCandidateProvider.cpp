#include "azookey/host/DictionaryCandidateProvider.h"

#include "azookey/learning/DictionaryCategory.h"

namespace azookey::host {
namespace {
// auto-word-registration-spec sections 14.4 / 14.12; category ids are the
// .azdic bit positions. Technical takes precedence over surface-derived English.
constexpr uint16_t kTechnicalCategories =
    learning::CategoryBit(learning::DictionaryCategory::ProductName) |
    learning::CategoryBit(learning::DictionaryCategory::Software) |
    learning::CategoryBit(learning::DictionaryCategory::Technical);
}  // namespace

std::vector<core::Candidate> DictionaryCandidates(const learning::DictionaryStore& store,
                                                  std::string_view reading,
                                                  learning::LookupMode mode, uint64_t now_epoch_sec,
                                                  size_t limit, bool include_user,
                                                  double user_word_default_score) {
  if (limit == 0) return {};
  learning::LookupContext ctx;
  ctx.mode = mode;
  ctx.max_results = limit;
  ctx.exclude_exact_key = mode == learning::LookupMode::PredictivePrefix;
  ctx.user_word_default_score = user_word_default_score;
  ctx.now_epoch_sec = now_epoch_sec;
  if (!include_user) ctx.excluded_layers = 1U << static_cast<unsigned>(learning::LayerId::User);
  std::vector<core::Candidate> result;
  for (const auto& entry : store.Lookup(reading, ctx)) {
    const bool local = entry.source >= learning::LayerId::User;
    result.push_back(
        {entry.surface, mode == learning::LookupMode::Exact ? std::string(reading) : entry.reading,
         entry.score,
         local ? core::CandidateSource::UserDictionary : core::CandidateSource::SystemDictionary,
         "dictionary", "",
         (entry.category_mask & kTechnicalCategories) ? core::CandidateTag::Technical
                                                      : core::CandidateTag::None});
    if (result.size() == limit) break;
  }
  return result;
}
}  // namespace azookey::host
