#include "azookey/learning/Reranker.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace azookey::learning {

std::vector<azookey::core::Candidate> Reranker::Apply(
    const std::string& reading, std::vector<azookey::core::Candidate> candidates,
    uint64_t now_epoch_sec, const std::atomic<bool>* cancel) const {
  const auto canceled = [cancel] { return cancel && cancel->load(std::memory_order_relaxed); };
  if (canceled()) return {};
  if (!store_) {
    return canceled() ? std::vector<azookey::core::Candidate>{} : std::move(candidates);
  }

  std::vector<azookey::core::Candidate> finite_candidates;
  finite_candidates.reserve(candidates.size());
  for (auto& c : candidates) {
    if (canceled()) return {};
    c.score += store_->Score(reading, c.surface, now_epoch_sec);
    if (!std::isfinite(c.score)) {
      continue;
    }
    finite_candidates.push_back(std::move(c));
  }
  if (canceled()) return {};
  candidates = std::move(finite_candidates);

  std::stable_sort(candidates.begin(), candidates.end(), [](const auto& l, const auto& r) {
    return l.score > r.score;
  });
  if (canceled()) return {};
  return candidates;
}

}  // namespace azookey::learning
