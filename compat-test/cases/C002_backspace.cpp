#include <Windows.h>

#include <string>

#include "runner/CaseSupport.h"
#include "runner/CompatTypes.h"

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
        if (!session.ClearEditor() ||
            !session.SendAscii(std::string(C002BackspaceScenario::kInput))) {
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
        } else if (IsExpectedC002BackspaceTransition(*before, *after)) {
          result.status = ResultStatus::Pass;
          result.reason_code = "expected-backspace-transition";
        } else {
          result.status = ResultStatus::Fail;
          result.reason_code = "unexpected-backspace-transition";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
