#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::core {

// DEV-1734 (docs/legacy-parity-spec.md section 5.4): picks which reading of an
// ambiguous "nn" + vowel spelling (RomajiKanaConverter::AmbiguousReadings) to
// convert, by how well each reading splits into dictionary words.

// A dictionary word that starts a reading suffix: the byte length of its
// reading and its frequency (0.0 to 1.0, higher is more common).
struct ReadingWordMatch {
  size_t length{};
  double frequency{};
};

// The dictionary words whose reading is a prefix of `suffix`. An empty function
// means no dictionary.
using ReadingWordLookup = std::function<std::vector<ReadingWordMatch>(std::string_view suffix)>;

struct AmbiguousReadingCosts {
  // Added for each dictionary word of a split, so fewer, longer words win.
  double word{0.5};
  // Each kana that no dictionary word covers.
  double unknown{1.5};
  // Another reading must cost less than the first by more than this.
  double margin{0.3};
};

// The cost of the cheapest split of `reading` into dictionary words (1 -
// frequency + word each) and uncovered kana (unknown each).
double ReadingSplitCost(std::string_view reading, const ReadingWordLookup& lookup,
                        const AmbiguousReadingCosts& costs = {});

// The index into `readings` to convert: 0 (the default reading) unless the
// cheapest other reading costs less than it by more than costs.margin. Ties go to
// the earlier reading. 0 for fewer than two readings or without a dictionary.
size_t ChooseAmbiguousReading(const std::vector<std::string>& readings,
                              const ReadingWordLookup& lookup,
                              const AmbiguousReadingCosts& costs = {});

}  // namespace azookey::core
