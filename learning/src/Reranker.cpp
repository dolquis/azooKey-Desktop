#include "azookey/learning/Reranker.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace azookey::learning {

std::vector<azookey::core::Candidate> Reranker::Apply(const std::string& reading,
                                                       std::vector<azookey::core::Candidate> candidates,
                                                       uint64_t now_epoch_sec) const {
  if (!store_) {
    return candidates;
  }

  std::vector<azookey::core::Candidate> finite_candidates;
  finite_candidates.reserve(candidates.size());
  for (auto& c : candidates) {
    c.score += store_->Score(reading, c.surface, now_epoch_sec);
    if (!std::isfinite(c.score)) {
      continue;
    }
    finite_candidates.push_back(std::move(c));
  }
  candidates = std::move(finite_candidates);

  std::stable_sort(candidates.begin(), candidates.end(), [](const auto& l, const auto& r) {
    return l.score > r.score;
  });
  return candidates;
}

}  // namespace azookey::learning
