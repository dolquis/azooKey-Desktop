#pragma once

#include "azookey/core/AmbiguousReadingChooser.h"
#include "azookey/core/Candidate.h"
#include "azookey/learning/DictionaryStore.h"

namespace azookey::host {
std::vector<core::Candidate> DictionaryCandidates(const learning::DictionaryStore& store,
                                                  std::string_view reading,
                                                  learning::LookupMode mode, uint64_t now_epoch_sec,
                                                  size_t limit = 32, bool include_user = true,
                                                  double user_word_default_score = 1.5);

// DEV-1734: the static layers' words for core::ChooseAmbiguousReading, the most
// frequent word per reading length. User words carry no frequency of their own,
// and a secure request must not consult them, so the user, auto-word and
// app-specific layers are left out. The caller keeps `store` locked and alive
// while the lookup is used.
core::ReadingWordLookup StaticReadingWordLookup(const learning::DictionaryStore& store);
}  // namespace azookey::host
