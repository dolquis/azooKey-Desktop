#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/core/SegmentPos.h"

namespace azookey::core {

enum class PunctuationKind { Comma, Period };

struct PunctuationGuard {
  enum class Field { PrevPos, NextHeadPos, PrevSem, NextHeadSem, SentenceFinal };
  Field field{Field::SentenceFinal};
  bool not_equal{false};
  SegmentPos pos{SegmentPos::Unknown};
  SegmentSemantic sem{SegmentSemantic::Unknown};
};

struct PunctuationRule {
  PunctuationKind kind{PunctuationKind::Comma};
  std::string match;
  double base_score{0.0};
  std::vector<PunctuationGuard> guards;
};

// The policy is a value, so a host can atomically replace it on a TSV reload.
class PunctuationRules {
 public:
  static PunctuationRules Default();
  // Starts with built-ins. Later rows with the same (kind, match) replace
  // earlier rows. Invalid rows are skipped and reported by one-based line.
  static PunctuationRules ParseAndMerge(std::string_view tsv,
                                        std::vector<size_t>* invalid_lines = nullptr);
  const std::vector<PunctuationRule>& rules() const { return rules_; }
  static bool MatchesGuard(const PunctuationRule& rule, SegmentPos prev_pos,
                           SegmentSemantic prev_sem, SegmentPos next_head_pos,
                           SegmentSemantic next_head_sem, bool sentence_final);

 private:
  std::vector<PunctuationRule> rules_;
};

}  // namespace azookey::core
