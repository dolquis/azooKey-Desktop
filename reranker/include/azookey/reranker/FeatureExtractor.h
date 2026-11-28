#pragma once

#include <array>
#include <string_view>

#include "azookey/core/Candidate.h"

namespace azookey::reranker {

using core::RerankSignals;

struct UserSignals {
  double user_frequency{};
  double recency_score{};
};

inline constexpr std::string_view kFeaturesV1InputName = "features_v1";
inline constexpr std::string_view kRerankScoreOutputName = "rerank_score";
inline constexpr std::array<std::string_view, 12> kFeatureNamesV1 = {
    "zenzai_score",     "dictionary_score", "user_frequency", "recency_score",
    "typo_confidence",  "app_profile_score", "candidate_length", "segment_count",
    "is_named_entity",  "is_user_dict",      "is_neologism",     "is_typo_corrected",
};

// Raw features in model input order. The ONNX graph owns any standardization.
std::array<float, 12> ExtractFeaturesV1(const RerankSignals& candidate, const UserSignals& user);

}  // namespace azookey::reranker
