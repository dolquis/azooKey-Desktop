#include "azookey/core/AmbiguousReadingChooser.h"

#include <algorithm>
#include <limits>

#include "azookey/core/Utf8.h"

namespace azookey::core {

double ReadingSplitCost(std::string_view reading, const ReadingWordLookup& lookup,
                        const AmbiguousReadingCosts& costs) {
  constexpr double kUnreached = std::numeric_limits<double>::infinity();
  // best[i]: the cheapest split of reading[0, i). Only scalar boundaries are reached.
  std::vector<double> best(reading.size() + 1, kUnreached);
  best[0] = 0.0;
  for (size_t begin = 0; begin < reading.size();) {
    size_t next = begin;
    char32_t codepoint{};
    if (!DecodeNextUtf8(reading, next, codepoint)) next = begin + 1;
    if (best[begin] != kUnreached) {
      best[next] = (std::min)(best[next], best[begin] + costs.unknown);
      if (lookup) {
        for (const auto& match : lookup(reading.substr(begin))) {
          if (match.length == 0 || match.length > reading.size() - begin) continue;
          const double frequency = std::clamp(match.frequency, 0.0, 1.0);
          double& end = best[begin + match.length];
          end = (std::min)(end, best[begin] + (1.0 - frequency) + costs.word);
        }
      }
    }
    begin = next;
  }
  return best.back();
}

size_t ChooseAmbiguousReading(const std::vector<std::string>& readings,
                              const ReadingWordLookup& lookup, const AmbiguousReadingCosts& costs) {
  if (readings.size() < 2 || !lookup) return 0;
  const double first = ReadingSplitCost(readings.front(), lookup, costs);
  size_t chosen = 0;
  double chosen_cost = first - costs.margin;
  for (size_t i = 1; i < readings.size(); ++i) {
    const double cost = ReadingSplitCost(readings[i], lookup, costs);
    if (cost < chosen_cost) {
      chosen = i;
      chosen_cost = cost;
    }
  }
  return chosen;
}

}  // namespace azookey::core
