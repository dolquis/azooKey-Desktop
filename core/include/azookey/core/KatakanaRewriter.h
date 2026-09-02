#pragma once

#include <string>
#include <vector>

#include "azookey/core/Candidate.h"

namespace azookey::core {

// Expands a pure hiragana reading into deterministic full-width and, when every
// character has a mapping, half-width katakana variants.
std::vector<Candidate> ExpandKatakanaCandidates(const std::string& reading);

}  // namespace azookey::core
