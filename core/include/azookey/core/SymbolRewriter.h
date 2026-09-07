#pragma once

#include <string>
#include <vector>

#include "azookey/core/Candidate.h"

namespace azookey::core {

// Append the first matching family, preserving all existing candidates.
// The caller owns the feature gate and excludes live/batch/prediction paths.
void AppendSymbolChain(std::vector<Candidate>& candidates, const std::string& reading);

// Inputs are already ranked; deduplicate before reserving the two tail budgets.
// Zero means unlimited total size, while each lookup tail is always capped at four.
std::vector<Candidate> MergeRewriterCandidates(std::vector<Candidate> ordinary,
                                               std::vector<Candidate> symbols,
                                               std::vector<Candidate> emoji, size_t max_candidates);

}  // namespace azookey::core
