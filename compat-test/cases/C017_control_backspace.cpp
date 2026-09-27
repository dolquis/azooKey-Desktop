#include <Windows.h>

#include "runner/CaseSupport.h"
#include "runner/CompatTypes.h"

namespace azookey::compat_test {

CaseDefinition MakeC017ControlBackspaceCase() {
  return {
      "C-017",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-017";
        result.status = ResultStatus::FailingSkip;
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        if (!session.ClearEditor() || !session.SendAscii("nihongo")) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto before = session.ReadEditorText();
        if (!before || *before != C002BackspaceScenario::kBefore) {
          result.reason_code = "control-backspace-preedit-unobservable";
          return result;
        }
        if (!session.SendModifiedKey({VK_CONTROL}, 'H')) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto after = session.ReadEditorText();
        if (!after) {
          result.reason_code = "text-pattern-unavailable";
        } else if (IsExpectedC002BackspaceTransition(*before, *after)) {
          result.status = ResultStatus::Pass;
          result.reason_code = "control-h-deleted-preedit-unit";
        } else {
          result.status = ResultStatus::Fail;
          result.reason_code = "control-h-preedit-transition-mismatch";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
