#include "azookey/core/SymbolRewriter.h"

#include <algorithm>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace azookey::core {

std::vector<Candidate> MergeRewriterCandidates(std::vector<Candidate> ordinary,
                                               std::vector<Candidate> symbols,
                                               std::vector<Candidate> emoji,
                                               size_t max_candidates) {
  std::unordered_set<std::string> seen;
  for (const auto& candidate : ordinary) seen.insert(candidate.surface);
  const auto unique_tail = [&](std::vector<Candidate>& tail) {
    std::vector<Candidate> unique;
    for (auto& candidate : tail) {
      if (seen.insert(candidate.surface).second) {
        unique.push_back(std::move(candidate));
        if (unique.size() == 4) break;
      }
    }
    tail = std::move(unique);
  };
  unique_tail(symbols);
  unique_tail(emoji);
  if (max_candidates != 0) {
    const auto tail_budget = max_candidates - (ordinary.empty() ? 0u : 1u);
    if (symbols.size() > tail_budget) symbols.resize(tail_budget);
    if (emoji.size() > tail_budget - symbols.size()) emoji.resize(tail_budget - symbols.size());
    const auto ordinary_budget = max_candidates - symbols.size() - emoji.size();
    if (ordinary.size() > ordinary_budget) ordinary.resize(ordinary_budget);
  }
  ordinary.insert(ordinary.end(), std::make_move_iterator(symbols.begin()),
                  std::make_move_iterator(symbols.end()));
  ordinary.insert(ordinary.end(), std::make_move_iterator(emoji.begin()),
                  std::make_move_iterator(emoji.end()));
  return ordinary;
}

}  // namespace azookey::core
