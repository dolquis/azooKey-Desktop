#include "runner/CompatTypes.h"

#include <Windows.h>

#include <chrono>
#include <thread>

namespace azookey::compat_test {

CaseDefinition MakeC001BasicInputCase() {
  return {
      "C-001",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-001";
        if (!session.ClearEditor() || !session.SendAscii("nihongo") ||
            !session.SendVirtualKey(VK_SPACE) || !session.SendVirtualKey(VK_RETURN)) {
          result.status = ResultStatus::FailingSkip;
          result.reason_code = "input-injection-failed";
          return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto text = session.ReadEditorText();
        if (!text) {
          result.status = ResultStatus::FailingSkip;
          result.reason_code = "text-pattern-unavailable";
        } else if (text->find(L"日本語") != std::wstring::npos) {
          result.status = ResultStatus::Pass;
          result.reason_code = "expected-commit-observed";
          session.MarkBaselineVerified();
        } else {
          result.status = ResultStatus::Fail;
          result.reason_code = "expected-commit-not-observed";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
