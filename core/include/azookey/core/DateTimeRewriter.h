#pragma once

#include <string>
#include <vector>

#include "azookey/core/Candidate.h"

namespace azookey::core {

// Local wall-clock time handed in by the caller (docs/candidate-rewriter-spec.md
// §20.2). core never reads a clock; the weekday is derived from the date.
struct LocalDateTime {
  int year{};
  int month{};
  int day{};
  int hour{};
  int minute{};
};

// Expands a reading that exactly matches the §20.3 table (`きょう`, `ことし`,
// `いま`, ...) into the §20.4 date, year or time candidates, in table order.
// Other readings and out-of-range times return no candidates.
std::vector<Candidate> ExpandDateTimeCandidates(const std::string& reading,
                                                const LocalDateTime& now);

}  // namespace azookey::core
