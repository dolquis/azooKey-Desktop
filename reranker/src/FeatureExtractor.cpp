#include "azookey/reranker/FeatureExtractor.h"

#include <limits>

namespace azookey::reranker {
namespace {

float ToFeatureFloat(double value) {
  constexpr double max = std::numeric_limits<float>::max();
  if (value > max) return std::numeric_limits<float>::infinity();
  if (value < -max) return -std::numeric_limits<float>::infinity();
  return static_cast<float>(value);
}

}  // namespace

std::array<float, 12> ExtractFeaturesV1(const RerankSignals& candidate, const UserSignals& user) {
  return {ToFeatureFloat(candidate.zenzai_avg_logprob.value_or(-10.0)),
          ToFeatureFloat(candidate.dictionary_score.value_or(0.0)),
          ToFeatureFloat(user.user_frequency),
          ToFeatureFloat(user.recency_score),
          ToFeatureFloat(candidate.typo_confidence.value_or(0.0)),
          0.0f,  // M48 tag boosts are applied after reranking.
          static_cast<float>(candidate.candidate_length),
          static_cast<float>(candidate.lattice_segment_count.value_or(1)),
          candidate.is_named_entity ? 1.0f : 0.0f,
          candidate.is_user_dict ? 1.0f : 0.0f,
          candidate.is_neologism ? 1.0f : 0.0f,
          candidate.is_typo_corrected ? 1.0f : 0.0f};
}

}  // namespace azookey::reranker
