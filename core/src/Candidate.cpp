#include "azookey/core/Candidate.h"

#include "azookey/core/Utf8.h"

namespace azookey::core {

size_t CountCandidateCodepoints(std::string_view surface) {
  size_t offset = 0;
  size_t count = 0;
  while (offset < surface.size()) {
    char32_t codepoint{};
    if (DecodeNextUtf8(surface, offset, codepoint)) ++count;
  }
  return count;
}

void PreserveRerankSignals(RerankSignals& winner, const RerankSignals& other) {
  if (!winner.zenzai_avg_logprob) winner.zenzai_avg_logprob = other.zenzai_avg_logprob;
  if (!winner.dictionary_score) winner.dictionary_score = other.dictionary_score;
  if (!winner.typo_confidence) winner.typo_confidence = other.typo_confidence;
  if (!winner.lattice_segment_count) winner.lattice_segment_count = other.lattice_segment_count;
  winner.is_named_entity |= other.is_named_entity;
  winner.is_user_dict |= other.is_user_dict;
  winner.is_neologism |= other.is_neologism;
  winner.is_typo_corrected |= other.is_typo_corrected;
}

}  // namespace azookey::core
