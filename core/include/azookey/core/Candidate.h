#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace azookey::core {

// docs/legacy-parity-spec.md §3.9. One tag per candidate; the values travel on
// the wire as CandidateField::tag, so only append.
enum class CandidateTag : uint8_t {
  None = 0,
  Polite = 1,
  Casual = 2,
  Technical = 3,
  English = 4,
  Kaomoji = 5,
  Idiom = 6,
};

enum class CandidateSource {
  SystemDictionary,
  UserDictionary,
  Model,
  Llm,
  Heuristic,
  Learning,
  Symbol,
  Emoji,
};

struct RerankSignals {
  std::optional<double> zenzai_avg_logprob{};
  std::optional<double> dictionary_score{};
  std::optional<double> typo_confidence{};
  size_t candidate_length{};
  std::optional<size_t> lattice_segment_count{};
  bool is_named_entity{};
  bool is_user_dict{};
  bool is_neologism{};
  bool is_typo_corrected{};
};

// Count Unicode scalar values in the candidate surface; invalid bytes are skipped.
size_t ComputeCandidateLength(std::string_view surface);

struct Candidate {
  std::string surface;
  std::string reading;
  double score{};
  CandidateSource source{CandidateSource::Heuristic};
  std::string debug_info;
  std::string description;
  CandidateTag tag{CandidateTag::None};
  RerankSignals rerank_signals{};
};

// Fold signals from another occurrence of the same surface into the ranking winner.
void PreserveRerankSignals(Candidate& representative, const Candidate& duplicate);

}  // namespace azookey::core
