#include <Windows.h>

#include <chrono>
#include <thread>

#include "runner/CaseSupport.h"
#include "runner/CompatTypes.h"

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
        if (IsCandidateNearCaretAtDpi(*candidate, *caret, GetDpiForWindow(session.window()))) {
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
