#pragma once

#include <string>

namespace azookey::core {

enum class CandidateSource {
  SystemDictionary,
  UserDictionary,
  Model,
  Llm,
  Heuristic,
  Learning,
};

struct Candidate {
  std::string surface;
  std::string reading;
  double score{};
  CandidateSource source{CandidateSource::Heuristic};
  std::string debug_info;
  std::string description;
};

}  // namespace azookey::core
