#include "runner/CompatTypes.h"

namespace azookey::compat_test {

CaseDefinition MakeC007SurrogatePairCase() {
  return {
      "C-007",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-007";
        result.status = ResultStatus::FailingSkip;
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        constexpr wchar_t kEmoji[] = {0xD83D, 0xDE00, L'\0'};
        if (!session.ClearEditor() || !session.SendUnicode(kEmoji)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto text = session.ReadEditorText();
        if (!text) {
          result.reason_code = "text-pattern-unavailable";
        } else if (text->find(kEmoji) != std::wstring::npos) {
          result.reason_code = "surrogate-pair-tip-path-unverified";
        } else {
          result.status = ResultStatus::Fail;
          result.reason_code = "surrogate-pair-not-observed";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
