#include <Windows.h>

#include <chrono>
#include <string_view>
#include <thread>

#include "runner/CompatTypes.h"

namespace azookey::compat_test {
namespace {

bool WaitForCandidateWindow(AutomationSession& session) {
  for (int attempt = 0; attempt < 30; ++attempt) {
    if (session.CandidateRect()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

}  // namespace

CaseDefinition MakeC015CandidateKeysCase() {
  return {
      "C-015",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-015";
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        // The standalone punctuation path has two local candidates: 、 and ,.
        // Unlike a dictionary reading, their order does not depend on the Host.
        if (!session.ClearEditor() || !session.SendVirtualKey(VK_OEM_COMMA)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto reading = session.ReadEditorText();
        if (!reading || *reading != L"、") {
          result.reason_code = "punctuation-fixture-unavailable";
          return result;
        }
        if (!session.SendVirtualKey(VK_SPACE)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        if (!WaitForCandidateWindow(session)) {
          result.reason_code = "punctuation-candidate-window-unobservable";
          return result;
        }
        struct Step {
          WORD key;
          WORD modifier;
          std::wstring_view expected;
          const char* mismatch_reason;
        };
        constexpr Step kSteps[] = {
            {VK_DOWN, 0, L",", "candidate-down-mismatch"},
            {VK_UP, 0, L"、", "candidate-up-mismatch"},
            {VK_SPACE, 0, L",", "candidate-space-cycle-mismatch"},
            {VK_SPACE, VK_SHIFT, L"、", "candidate-shift-space-mismatch"},
            {'N', VK_CONTROL, L",", "candidate-control-n-mismatch"},
            {'P', VK_CONTROL, L"、", "candidate-control-p-mismatch"},
        };
        for (const auto& step : kSteps) {
          const bool sent = step.modifier ? session.SendModifiedKey({step.modifier}, step.key)
                                          : session.SendVirtualKey(step.key);
          if (!sent) {
            result.reason_code = session.input_failure_reason();
            return result;
          }
          const auto text = session.ReadEditorText();
          if (!text) {
            result.reason_code = "candidate-preedit-unobservable";
            return result;
          }
          if (*text != step.expected || !session.CandidateRect()) {
            result.status = ResultStatus::Fail;
            result.reason_code = step.mismatch_reason;
            return result;
          }
        }
        if (!session.SendVirtualKey('2')) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto digit_commit = session.ReadEditorText();
        if (!digit_commit) {
          result.reason_code = "committed-text-unobservable";
          return result;
        }
        if (*digit_commit != L"," || session.CandidateRect()) {
          result.status = ResultStatus::Fail;
          result.reason_code = "candidate-digit-commit-mismatch";
          return result;
        }

        if (!session.ClearEditor() || !session.SendVirtualKey(VK_OEM_COMMA)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto escape_reading = session.ReadEditorText();
        if (!escape_reading || *escape_reading != L"、") {
          result.reason_code = "punctuation-fixture-unavailable";
          return result;
        }
        if (!session.SendVirtualKey(VK_SPACE)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        if (!WaitForCandidateWindow(session)) {
          result.reason_code = "punctuation-candidate-window-unobservable";
          return result;
        }
        if (!session.SendVirtualKey(VK_DOWN) || !session.SendVirtualKey(VK_ESCAPE)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto after_first_escape = session.ReadEditorText();
        if (!after_first_escape) {
          result.reason_code = "candidate-preedit-unobservable";
          return result;
        }
        if (*after_first_escape != L"、" || session.CandidateRect()) {
          result.status = ResultStatus::Fail;
          result.reason_code = "candidate-first-escape-mismatch";
          return result;
        }
        if (!session.SendVirtualKey(VK_ESCAPE)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto after_second_escape = session.ReadEditorText();
        if (!after_second_escape) {
          result.reason_code = "candidate-preedit-unobservable";
          return result;
        }
        if (!after_second_escape->empty() || session.CandidateRect()) {
          result.status = ResultStatus::Fail;
          result.reason_code = "candidate-second-escape-mismatch";
          return result;
        }
        result.status = ResultStatus::Pass;
        result.reason_code = "candidate-keys-and-two-stage-escape-observed";
        return result;
      },
  };
}

}  // namespace azookey::compat_test
