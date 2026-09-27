#include <Windows.h>

#include <chrono>
#include <thread>

#include "runner/CaseSupport.h"
#include "runner/CompatTypes.h"

namespace azookey::compat_test {
namespace {

std::optional<std::wstring> WaitForText(AutomationSession& session, std::wstring_view expected) {
  for (int attempt = 0; attempt < 30; ++attempt) {
    auto text = session.ReadEditorText();
    if (!text || *text == expected) return text;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return session.ReadEditorText();
}

}  // namespace

CaseDefinition MakeC018LiveConversionCase() {
  return {
      "C-018",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-018";
        result.status = ResultStatus::FailingSkip;
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        const auto setting = ReadLiveConversionSetting();
        if (setting != LiveConversionSetting::Enabled) {
          result.reason_code = setting == LiveConversionSetting::Disabled
                                   ? "live-conversion-disabled"
                                   : "live-conversion-setting-unavailable";
          return result;
        }
        if (!session.ClearEditor() || !session.SendAscii("nihongo")) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto preedit = WaitForText(session, L"日本語");
        if (!preedit) {
          result.reason_code = "live-preedit-unobservable";
          return result;
        }
        if (*preedit != L"日本語" || session.CandidateRect()) {
          result.status = ResultStatus::Fail;
          result.reason_code = "live-preedit-mismatch";
          return result;
        }
        if (!session.SendVirtualKey(VK_RETURN)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto committed = session.ReadEditorText();
        if (!committed) {
          result.reason_code = "live-commit-unobservable";
          return result;
        }
        if (*committed != L"日本語") {
          result.status = ResultStatus::Fail;
          result.reason_code = "live-commit-mismatch";
          return result;
        }
        if (!session.ClearEditor() || !session.SendAscii("nihongo")) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto before_backspace = WaitForText(session, L"日本語");
        if (!before_backspace) {
          result.reason_code = "live-backspace-preedit-unobservable";
          return result;
        }
        if (*before_backspace != L"日本語") {
          result.status = ResultStatus::Fail;
          result.reason_code = "live-backspace-preedit-mismatch";
          return result;
        }
        if (!session.SendVirtualKey(VK_BACK)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        auto after_backspace = session.ReadEditorText();
        for (int attempt = 0;
             after_backspace && *after_backspace == *before_backspace && attempt < 30; ++attempt) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          after_backspace = session.ReadEditorText();
        }
        if (!after_backspace) {
          result.reason_code = "live-backspace-result-unobservable";
          return result;
        }
        if (after_backspace->empty() || *after_backspace == *before_backspace) {
          result.status = ResultStatus::Fail;
          result.reason_code = "live-backspace-transition-mismatch";
          return result;
        }
        // Prediction Esc has its own meaning; close that separate window first.
        if (!session.DismissPredictionWindow()) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        if (!session.SendVirtualKey(VK_ESCAPE)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto after_escape = WaitForText(session, L"");
        if (!after_escape) {
          result.reason_code = "live-escape-result-unobservable";
        } else if (!after_escape->empty()) {
          result.status = ResultStatus::Fail;
          result.reason_code = "live-escape-did-not-clear-composition";
        } else {
          result.status = ResultStatus::Pass;
          result.reason_code = "live-preedit-enter-backspace-escape-observed";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
