#include <Windows.h>

#include <array>
#include <chrono>
#include <thread>
#include <vector>

#include "runner/CompatTypes.h"

namespace azookey::compat_test {
namespace {

bool IsTargetForeground(HWND window) {
  const HWND foreground = GetForegroundWindow();
  return foreground == window || (foreground && IsChild(window, foreground));
}

bool SendFastNihongoSpace(AutomationSession& session, const char** failure_reason) {
  if (!session.FocusEditor()) {
    *failure_reason = session.input_failure_reason();
    return false;
  }
  if (!IsTargetForeground(session.window())) {
    *failure_reason = "focus-lost";
    return false;
  }
  constexpr std::array<WORD, 8> kKeys = {'N', 'I', 'H', 'O', 'N', 'G', 'O', VK_SPACE};
  std::vector<INPUT> inputs;
  inputs.reserve(kKeys.size() * 2);
  for (const WORD key : kKeys) {
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = key;
    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    inputs.push_back(down);
    inputs.push_back(up);
  }
  const UINT input_count = static_cast<UINT>(inputs.size());
  if (SendInput(input_count, inputs.data(), sizeof(INPUT)) != input_count) {
    *failure_reason = "input-injection-failed";
    return false;
  }
  return true;
}

bool WaitForCandidateWindow(AutomationSession& session) {
  for (int attempt = 0; attempt < 30; ++attempt) {
    if (session.CandidateRect()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

}  // namespace

CaseDefinition MakeC016FastInputCase() {
  return {
      "C-016",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-016";
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        if (!session.ClearEditor()) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const char* input_reason = nullptr;
        if (!SendFastNihongoSpace(session, &input_reason)) {
          result.reason_code = input_reason;
          return result;
        }
        if (!WaitForCandidateWindow(session)) {
          result.status = ResultStatus::Fail;
          result.reason_code = "fast-input-candidate-window-not-found";
          return result;
        }
        const auto selected = session.ReadEditorText();
        if (!selected) {
          result.reason_code = "candidate-preedit-unobservable";
          return result;
        }
        if (*selected != L"日本語") {
          result.status = ResultStatus::Fail;
          result.reason_code = "fast-input-candidate-mismatch";
          return result;
        }
        if (!session.SendVirtualKey(VK_RETURN)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto committed = session.ReadEditorText();
        if (!committed) {
          result.reason_code = "committed-text-unobservable";
          return result;
        }
        if (*committed != L"日本語" || session.CandidateRect()) {
          result.status = ResultStatus::Fail;
          result.reason_code = "fast-input-commit-mismatch";
          return result;
        }
        if (!session.SendAscii("siro")) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto resumed = session.ReadEditorText();
        if (!resumed) {
          result.reason_code = "recovery-preedit-unobservable";
          return result;
        }
        if (*resumed != L"日本語しろ") {
          result.status = ResultStatus::Fail;
          result.reason_code = "fast-input-recovery-preedit-mismatch";
          return result;
        }
        if (!session.SendVirtualKey(VK_RETURN)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto recovered_commit = session.ReadEditorText();
        if (!recovered_commit) {
          result.reason_code = "recovery-commit-unobservable";
          return result;
        }
        if (*recovered_commit != L"日本語しろ") {
          result.status = ResultStatus::Fail;
          result.reason_code = "fast-input-recovery-commit-mismatch";
          return result;
        }
        result.status = ResultStatus::Pass;
        result.reason_code = "fast-input-candidate-and-recovery-observed";
        return result;
      },
  };
}

}  // namespace azookey::compat_test
