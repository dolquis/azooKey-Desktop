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
constexpr uint16_t kNamedEntityCategories =
    learning::CategoryBit(learning::DictionaryCategory::PersonName) |
    learning::CategoryBit(learning::DictionaryCategory::PlaceName) |
    learning::CategoryBit(learning::DictionaryCategory::StationName) |
    learning::CategoryBit(learning::DictionaryCategory::ProductName) |
    learning::CategoryBit(learning::DictionaryCategory::CompanyOrg);
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
  constexpr uint16_t kUserLayer = 1U << static_cast<unsigned>(learning::LayerId::User);
  if (!include_user) ctx.excluded_layers = kUserLayer;
  std::vector<core::Candidate> result;
  for (const auto& entry : store.Lookup(reading, ctx)) {
    const bool local = entry.source >= learning::LayerId::User;
    result.push_back(
        {entry.surface, mode == learning::LookupMode::Exact ? std::string(reading) : entry.reading,
         entry.score,
         local ? core::CandidateSource::UserDictionary : core::CandidateSource::SystemDictionary,
         "dictionary", "",
         (entry.category_mask & kTechnicalCategories) ? core::CandidateTag::Technical
                                                      : core::CandidateTag::None,
         {.dictionary_score = entry.score,
          .candidate_length = core::CountCandidateCodepoints(entry.surface),
          .is_named_entity = (entry.category_mask & kNamedEntityCategories) != 0,
          .is_user_dict = (entry.sources & kUserLayer) != 0,
          .is_neologism = (entry.category_mask &
                          learning::CategoryBit(learning::DictionaryCategory::Neologism)) != 0}});
    if (result.size() == limit) break;
  }
  return result;
}
}  // namespace azookey::host
