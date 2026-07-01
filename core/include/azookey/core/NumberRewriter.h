#pragma once

#include <string>
#include <vector>

#include "azookey/core/Candidate.h"

namespace azookey::core {

// Expands a pure ASCII decimal reading into deterministic numeric variants.
// Mixed text, empty input, and values outside uint64_t return no candidates.
std::vector<Candidate> ExpandNumberCandidates(const std::string& reading);

}  // namespace azookey::core
