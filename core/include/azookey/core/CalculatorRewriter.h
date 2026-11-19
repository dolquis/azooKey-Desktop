#pragma once

#include <string>
#include <vector>

#include "azookey/core/Candidate.h"

namespace azookey::core {

// Evaluates a reading that ends in `＝` or `=` and holds an arithmetic
// expression with at least one binary operator (docs/candidate-rewriter-spec.md
// §20.6), returning the result and the ASCII expression candidates in that
// order. Readings outside the grammar, division by zero, overflow and the
// length and nesting limits return no candidates.
std::vector<Candidate> ExpandCalculatorCandidates(const std::string& reading);

}  // namespace azookey::core
