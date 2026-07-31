#include "runner/CompatTypes.h"

#include <Windows.h>

namespace azookey::compat_test {

CaseDefinition MakeC002BackspaceCase() {
  return {
      "C-002",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-002";
        if (!session.baseline_verified()) {
          result.status = ResultStatus::FailingSkip;
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        if (!session.ClearEditor() || !session.SendAscii("nihongo")) {
          result.status = ResultStatus::FailingSkip;
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto before = session.ReadEditorText();
        if (!session.SendVirtualKey(VK_BACK)) {
          result.status = ResultStatus::FailingSkip;
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto after = session.ReadEditorText();
        if (!before || !after || before->empty()) {
          result.status = ResultStatus::FailingSkip;
          result.reason_code = "preedit-text-unobservable";
        } else if (before->size() == after->size() + 1) {
          result.status = ResultStatus::Pass;
          result.reason_code = "preedit-shortened";
        } else {
          result.status = ResultStatus::Fail;
          result.reason_code = "preedit-not-shortened";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
