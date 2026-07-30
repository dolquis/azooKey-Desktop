#include "runner/CompatTypes.h"

#include <Windows.h>

namespace azookey::compat_test {

CaseDefinition MakeC003EscapeCase() {
  return {
      "C-003",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-003";
        if (!session.baseline_verified()) {
          result.status = ResultStatus::FailingSkip;
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        if (!session.ClearEditor() || !session.SendAscii("nihongo")) {
          result.status = ResultStatus::FailingSkip;
          result.reason_code = "input-injection-failed";
          return result;
        }
        const auto before = session.ReadEditorText();
        if (!session.SendVirtualKey(VK_ESCAPE)) {
          result.status = ResultStatus::FailingSkip;
          result.reason_code = "input-injection-failed";
          return result;
        }
        const auto after = session.ReadEditorText();
        if (!before || !after || before->empty()) {
          result.status = ResultStatus::FailingSkip;
          result.reason_code = "preedit-text-unobservable";
        } else if (after->empty()) {
          result.status = ResultStatus::Pass;
          result.reason_code = "composition-cleared";
        } else {
          result.status = ResultStatus::Fail;
          result.reason_code = "composition-remained";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
