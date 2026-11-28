#pragma once

#include <map>
#include <utility>

#include "azookey/core/Candidate.h"
#include "azookey/learning/DictionaryStore.h"

namespace azookey::host {
using DictionaryCategoryMasks = std::map<std::pair<std::string, std::string>, uint16_t>;

std::vector<core::Candidate> DictionaryCandidates(const learning::DictionaryStore& store,
                                                  std::string_view reading,
                                                  learning::LookupMode mode, uint64_t now_epoch_sec,
                                                  size_t limit = 32, bool include_user = true,
                                                  double user_word_default_score = 1.5,
                                                  DictionaryCategoryMasks* categories = nullptr);
}  // namespace azookey::host
