#include "runner/CompatTypes.h"

namespace azookey::compat_test {

CaseDefinition MakeC008UndoRedoCase() {
  return {
      "C-008",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-008";
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        if (!session.ClearEditor() || !session.SendAscii("nihongo") ||
            !session.SendVirtualKey(VK_SPACE) || !session.SendVirtualKey(VK_RETURN)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto committed = session.ReadEditorText();
        if (!session.SendModifiedKey({VK_CONTROL}, 'Z')) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto undone = session.ReadEditorText();
        if (!session.SendModifiedKey({VK_CONTROL}, 'Y')) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto redone = session.ReadEditorText();
        if (!committed || !undone || !redone || committed->empty()) {
          result.reason_code = "undo-redo-text-unobservable";
        } else if (undone->empty() && *redone == *committed) {
          result.status = ResultStatus::Pass;
          result.reason_code = "undo-redo-roundtrip-observed";
        } else {
          result.status = ResultStatus::Fail;
          result.reason_code = "undo-redo-roundtrip-mismatch";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
