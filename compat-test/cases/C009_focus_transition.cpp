#include <chrono>
#include <thread>

#include "runner/CompatTypes.h"

namespace azookey::compat_test {

CaseDefinition MakeC009FocusTransitionCase() {
  return {
      "C-009",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-009";
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        if (!session.ClearEditor() || !session.SendAscii("nihongo")) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const HWND shell = GetShellWindow();
        if (!shell || !SetForegroundWindow(shell)) {
          result.reason_code = "alternate-focus-target-unavailable";
          return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (GetForegroundWindow() == session.window()) {
          result.reason_code = "focus-transition-not-observed";
          return result;
        }
        if (!IsWindow(session.window()) || !session.FocusEditor()) {
          result.status = ResultStatus::Fail;
          result.reason_code = "target-lost-after-focus-transition";
          return result;
        }
        DWORD_PTR response = 0;
        const bool responsive = SendMessageTimeoutW(session.window(), WM_NULL, 0, 0,
                                                    SMTO_ABORTIFHUNG, 2000, &response) != 0;
        const auto text = session.ReadEditorText();
        if (responsive && text) {
          result.status = ResultStatus::Pass;
          result.reason_code = "focus-transition-safe";
        } else {
          result.status = ResultStatus::Fail;
          result.reason_code = "target-unresponsive-after-focus-transition";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
