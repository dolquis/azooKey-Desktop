#pragma once

#include "azookey/core/Candidate.h"
#include "azookey/learning/DictionaryStore.h"

namespace azookey::host {
std::vector<core::Candidate> DictionaryCandidates(const learning::DictionaryStore& store,
                                                  std::string_view reading,
                                                  learning::LookupMode mode, uint64_t now_epoch_sec,
                                                  size_t limit = 32, bool include_user = true);
}  // namespace azookey::host
