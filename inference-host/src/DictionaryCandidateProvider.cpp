#include "azookey/host/DictionaryCandidateProvider.h"

namespace azookey::host {
std::vector<core::Candidate> DictionaryCandidates(const learning::DictionaryStore& store,
                                                  std::string_view reading,
                                                  learning::LookupMode mode, uint64_t now_epoch_sec,
                                                  size_t limit, bool include_user) {
  learning::LookupContext ctx;
  ctx.mode = mode;
  ctx.max_results = limit;
  ctx.now_epoch_sec = now_epoch_sec;
  if (!include_user) ctx.excluded_layers = 1U << static_cast<unsigned>(learning::LayerId::User);
  std::vector<core::Candidate> result;
  for (const auto& entry : store.Lookup(reading, ctx)) {
    const bool local = entry.source >= learning::LayerId::User;
    result.push_back(
        {entry.surface, entry.normalized_reading, entry.score,
         local ? core::CandidateSource::UserDictionary : core::CandidateSource::SystemDictionary,
         "dictionary"});
  }
  return result;
}
}  // namespace azookey::host
