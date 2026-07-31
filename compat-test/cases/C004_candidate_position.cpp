#include "runner/CompatTypes.h"

#include <Windows.h>

#include <chrono>
#include <cmath>
#include <thread>

namespace azookey::compat_test {

CaseDefinition MakeC004CandidatePositionCase() {
  return {
      "C-004",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-004";
        if (!session.baseline_verified()) {
          result.status = ResultStatus::FailingSkip;
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        if (!session.ClearEditor() || !session.SendAscii("nihongo") ||
            !session.SendVirtualKey(VK_SPACE)) {
          result.status = ResultStatus::FailingSkip;
          result.reason_code = session.input_failure_reason();
          return result;
        }
        std::optional<RECT> candidate;
        for (int attempt = 0; attempt < 30 && !candidate; ++attempt) {
          candidate = session.CandidateRect();
          if (!candidate) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!candidate) {
          result.status = ResultStatus::Fail;
          result.reason_code = "candidate-window-not-found";
          return result;
        }
        const auto caret = session.CaretRect();
        if (!caret) {
          result.status = ResultStatus::FailingSkip;
          result.reason_code = "caret-rectangle-unavailable";
          return result;
        }
        constexpr LONG kHorizontalTolerance = 64;
        constexpr LONG kMaximumVerticalGap = 100;
        const bool horizontally_near =
            std::abs(candidate->left - caret->left) <= kHorizontalTolerance;
        const bool below_caret = candidate->top >= caret->bottom - 4 &&
                                 candidate->top - caret->bottom <= kMaximumVerticalGap;
        if (horizontally_near && below_caret) {
          result.status = ResultStatus::Pass;
          result.reason_code = "candidate-near-caret";
        } else {
          result.status = ResultStatus::Fail;
          result.reason_code = "candidate-position-out-of-range";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
