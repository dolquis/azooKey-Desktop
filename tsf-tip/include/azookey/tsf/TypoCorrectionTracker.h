#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/core/Utf8.h"

namespace azookey::tsf {

struct TypoCorrectionPair {
  std::string wrong_reading;
  std::string correct_reading;
};

// Owner-thread value state for typo-correction-learning-spec.md sections 4/6.
// Copying this tracker lets an unsuccessful TSF edit restore its key state.
// The caller handles privacy resets; Host settings remain the learning gate.
class TypoCorrectionTracker {
 public:
  bool CanArmPostCommitBackspace() const {
    return first_key_after_commit_ && pre_correction_reading_.empty();
  }

  // Every key consumes the one-key window, even when it does not arm a retry.
  void BeginKey(bool backspace, bool preedit_empty) {
    if (!first_key_after_commit_) return;
    const bool arm = backspace && preedit_empty && CanArmPostCommitBackspace();
    first_key_after_commit_ = false;
    if (arm) {
      pre_correction_reading_ = last_committed_reading_;
    }
    last_committed_reading_.clear();
  }

  // Called immediately before deleting a kana character. A backspace burst or
  // a backspace during retyping must not replace the original wrong reading.
  void BeforeBackspace(const std::string& reading) {
    if (pre_correction_reading_.empty() && !reading.empty()) {
      pre_correction_reading_ = reading;
    }
  }

  // Call only after TSF committed successfully. Every successful commit closes
  // the old correction and opens a fresh one-key retyping window.
  std::optional<TypoCorrectionPair> Commit(const std::string& final_reading) {
    std::optional<TypoCorrectionPair> pair;
    if (IsLearnablePair(pre_correction_reading_, final_reading)) {
      pair = TypoCorrectionPair{pre_correction_reading_, final_reading};
    }
    pre_correction_reading_.clear();
    last_committed_reading_ = final_reading;
    first_key_after_commit_ = true;
    return pair;
  }

  void Reset() {
    pre_correction_reading_.clear();
    last_committed_reading_.clear();
    first_key_after_commit_ = false;
  }

 private:
  // Match TypoCorrectionStore's bound before the quadratic distance step.
  static constexpr size_t kMaxReadingLength = 64;

  static std::vector<char32_t> DecodeReading(std::string_view reading) {
    std::vector<char32_t> codepoints;
    size_t offset = 0;
    char32_t codepoint = 0;
    while (offset < reading.size() && codepoints.size() <= kMaxReadingLength) {
      // Core consumes an invalid byte as one codepoint, as the Host does.
      core::DecodeNextUtf8(reading, offset, codepoint);
      codepoints.push_back(codepoint);
    }
    return codepoints;
  }

  static bool IsLearnablePair(const std::string& wrong, const std::string& correct) {
    if (wrong.empty() || correct.empty() || wrong == correct) return false;
    const auto lhs = DecodeReading(wrong);
    const auto rhs = DecodeReading(correct);
    if (lhs.size() < 2 || rhs.size() < 2 || lhs.size() > kMaxReadingLength ||
        rhs.size() > kMaxReadingLength) {
      return false;
    }
    const size_t relative = static_cast<size_t>(
        std::ceil(static_cast<double>(std::max(lhs.size(), rhs.size())) * 0.34));
    const size_t limit = std::min<size_t>(3, std::max<size_t>(1, relative));
    const size_t difference =
        lhs.size() > rhs.size() ? lhs.size() - rhs.size() : rhs.size() - lhs.size();
    if (difference > limit) return false;

    std::vector<size_t> previous(rhs.size() + 1);
    std::vector<size_t> current(rhs.size() + 1);
    for (size_t j = 0; j <= rhs.size(); ++j) previous[j] = j;
    for (size_t i = 1; i <= lhs.size(); ++i) {
      current[0] = i;
      for (size_t j = 1; j <= rhs.size(); ++j) {
        const size_t substitution = previous[j - 1] + (lhs[i - 1] == rhs[j - 1] ? 0 : 1);
        current[j] = std::min({current[j - 1] + 1, previous[j] + 1, substitution});
      }
      previous.swap(current);
    }
    return previous[rhs.size()] <= limit;
  }

  std::string pre_correction_reading_;
  std::string last_committed_reading_;
  bool first_key_after_commit_ = false;
};

}  // namespace azookey::tsf
