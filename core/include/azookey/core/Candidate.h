#pragma once

#include <cstdint>
#include <string>

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

struct Candidate {
  std::string surface;
  std::string reading;
  double score{};
  CandidateSource source{CandidateSource::Heuristic};
  std::string debug_info;
  std::string description;
  CandidateTag tag{CandidateTag::None};
};

}  // namespace azookey::core
