#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/core/Candidate.h"

namespace azookey::core {

std::string NormalizeRewriterReading(std::string_view input);
std::string NormalizeEmojiTrigger(std::string_view input);

// No I/O or mutable search state. A Host may share one immutable index.
class RewriterIndex {
 public:
  explicit RewriterIndex(CandidateSource source) : source_(source) {}
  // Invalid rows are skipped; the result is their count. Replaces previous data.
  size_t Parse(std::string_view tsv);
  std::vector<Candidate> LookupReading(std::string_view reading) const;
  std::vector<Candidate> SearchTrigger(std::string_view query, size_t limit) const;
  size_t size() const { return entries_.size(); }
  size_t EstimatedMemoryBytes() const;

 private:
  struct Entry {
    std::string surface;
    std::string name;
    uint32_t rank{};
    std::vector<std::string> triggers;
  };
  Candidate ToCandidate(size_t index, std::string_view reading) const;
  CandidateSource source_;
  std::vector<Entry> entries_;
  std::map<std::string, std::vector<size_t>, std::less<>> readings_;
};

}  // namespace azookey::core
