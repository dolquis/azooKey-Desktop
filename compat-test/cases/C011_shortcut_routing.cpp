#include <algorithm>
#include <chrono>
#include <cwchar>
#include <iterator>
#include <thread>
#include <vector>

#include "runner/ClipboardIsolation.h"
#include "runner/CompatTypes.h"

namespace azookey::compat_test {
namespace {

bool IsExplorerDialog(HWND window) {
  if (!window || !IsWindow(window)) return false;
  wchar_t class_name[64]{};
  if (GetClassNameW(window, class_name, static_cast<int>(std::size(class_name))) <= 0 ||
      std::wstring_view(class_name) != L"#32770") {
    return false;
  }
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (!process) return false;
  wchar_t image_path[32768]{};
  DWORD length = static_cast<DWORD>(std::size(image_path));
  const bool queried = QueryFullProcessImageNameW(process, 0, image_path, &length) != FALSE;
  CloseHandle(process);
  if (!queried) return false;
  const wchar_t* file_name = std::wcsrchr(image_path, L'\\');
  file_name = file_name ? file_name + 1 : image_path;
  return _wcsicmp(file_name, L"explorer.exe") == 0;
}

std::vector<HWND> SnapshotExplorerDialogs() {
  std::vector<HWND> dialogs;
  EnumWindows(
      [](HWND window, LPARAM value) -> BOOL {
        if (IsExplorerDialog(window)) {
          reinterpret_cast<std::vector<HWND>*>(value)->push_back(window);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&dialogs));
  return dialogs;
}

HWND FindNewExplorerDialog(const std::vector<HWND>& existing) {
  struct Search {
    const std::vector<HWND>* existing;
    HWND result{};
  } search{&existing};
  EnumWindows(
      [](HWND window, LPARAM value) -> BOOL {
        auto* search = reinterpret_cast<Search*>(value);
        if (IsWindowVisible(window) && IsExplorerDialog(window) &&
            std::find(search->existing->begin(), search->existing->end(), window) ==
                search->existing->end()) {
          search->result = window;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
  return search.result;
}

class DialogCloseGuard {
 public:
  explicit DialogCloseGuard(HWND window) : window_(window) {}
  ~DialogCloseGuard() { Close(); }

  bool Close() {
    if (!window_ || !IsWindow(window_)) return true;
    PostMessageW(window_, WM_CLOSE, 0, 0);
    for (int attempt = 0; attempt < 20 && IsWindow(window_); ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !IsWindow(window_);
  }

 private:
  HWND window_{};
};

class MenuCancelGuard {
 public:
  explicit MenuCancelGuard(HWND window) : window_(window) {}
  ~MenuCancelGuard() {
    if (IsWindow(window_)) PostMessageW(window_, WM_CANCELMODE, 0, 0);
  }

 private:
  HWND window_{};
};

}  // namespace

CaseDefinition MakeC011ShortcutRoutingCase() {
  return {
      "C-011",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-011";
        result.status = ResultStatus::FailingSkip;
        SystemClipboardAccess clipboard;
        std::string action_reason = "shortcut-routing-not-observed";
        const auto isolation =
            RunWithClipboardIsolation(clipboard, L"azookey-clipboard-test", [&]() {
              constexpr std::wstring_view kCopyText = L"azookey-shortcut-test";
              constexpr std::wstring_view kPasteText = L"azookey-clipboard-test";
              if (!session.ClearEditor() || !session.SendUnicode(kCopyText)) {
                action_reason = session.input_failure_reason();
                return ClipboardActionStatus::FailingSkip;
              }
              if (!session.SendModifiedKey({VK_CONTROL}, 'A') ||
                  !session.SendModifiedKey({VK_CONTROL}, 'C')) {
                action_reason = session.input_failure_reason();
                return ClipboardActionStatus::FailingSkip;
              }
              const auto copied = clipboard.ReadUnicodeText();
              if (!copied) {
                action_reason = "clipboard-observation-unavailable";
                return ClipboardActionStatus::FailingSkip;
              }
              if (*copied != kCopyText) {
                action_reason = "control-copy-routing-not-observed";
                return ClipboardActionStatus::Failed;
              }
              if (!clipboard.ReplaceWithDeterministicText(kPasteText)) {
                action_reason = "clipboard-test-data-unavailable";
                return ClipboardActionStatus::FailingSkip;
              }
              if (!session.SendModifiedKey({VK_CONTROL}, 'A') ||
                  !session.SendModifiedKey({VK_CONTROL}, 'V')) {
                action_reason = session.input_failure_reason();
                return ClipboardActionStatus::FailingSkip;
              }
              const auto pasted = session.ReadEditorText();
              if (!pasted) {
                action_reason = "text-pattern-unavailable";
                return ClipboardActionStatus::FailingSkip;
              }
              if (*pasted != kPasteText) {
                action_reason = "control-v-routing-not-observed";
                return ClipboardActionStatus::Failed;
              }
              if (!session.SendModifiedKey({VK_CONTROL}, 'L')) {
                action_reason = session.input_failure_reason();
                return ClipboardActionStatus::FailingSkip;
              }
              const auto after_control_l = session.ReadEditorText();
              if (!after_control_l) {
                action_reason = "text-pattern-unavailable";
                return ClipboardActionStatus::FailingSkip;
              }
              if (*after_control_l != kPasteText) {
                action_reason = "control-l-routing-not-observed";
                return ClipboardActionStatus::Failed;
              }
              if (!session.SendModifiedKey({VK_CONTROL}, 'S')) {
                action_reason = session.input_failure_reason();
                return ClipboardActionStatus::FailingSkip;
              }
              const auto after_control_s = session.ReadEditorText();
              if (!after_control_s) {
                action_reason = "text-pattern-unavailable";
                return ClipboardActionStatus::FailingSkip;
              }
              if (*after_control_s != kPasteText) {
                action_reason = "control-s-routing-not-observed";
                return ClipboardActionStatus::Failed;
              }
              MenuCancelGuard menu_cleanup(session.window());
              if (!session.SendModifiedKey({VK_MENU}, 'F') || !session.SendVirtualKey(VK_ESCAPE)) {
                action_reason = session.input_failure_reason();
                return ClipboardActionStatus::FailingSkip;
              }
              const auto after_alt = session.ReadEditorText();
              if (!after_alt) {
                action_reason = "text-pattern-unavailable";
                return ClipboardActionStatus::FailingSkip;
              }
              if (*after_alt != kPasteText) {
                action_reason = "alt-menu-routing-not-observed";
                return ClipboardActionStatus::Failed;
              }
              const auto existing_dialogs = SnapshotExplorerDialogs();
              if (!session.SendModifiedKey({VK_LWIN}, 'R')) {
                action_reason = session.input_failure_reason();
                return ClipboardActionStatus::FailingSkip;
              }
              HWND run_dialog = nullptr;
              for (int attempt = 0; attempt < 30 && !run_dialog; ++attempt) {
                run_dialog = FindNewExplorerDialog(existing_dialogs);
                if (!run_dialog) std::this_thread::sleep_for(std::chrono::milliseconds(100));
              }
              DialogCloseGuard dialog_cleanup(run_dialog);
              if (!run_dialog || GetForegroundWindow() != run_dialog) {
                action_reason = "win-shortcut-routing-not-observed";
                return ClipboardActionStatus::Failed;
              }
              if (!dialog_cleanup.Close()) {
                action_reason = "win-dialog-close-failed";
                return ClipboardActionStatus::Failed;
              }
              if (!session.FocusEditor()) {
                action_reason = session.input_failure_reason();
                return ClipboardActionStatus::FailingSkip;
              }
              return ClipboardActionStatus::Completed;
            });

        switch (isolation) {
          case ClipboardIsolationStatus::Completed:
            result.status = ResultStatus::Pass;
            result.reason_code = "shortcuts-routed-and-clipboard-restored";
            break;
          case ClipboardIsolationStatus::BackupUnavailable:
            result.reason_code = "clipboard-backup-unavailable";
            break;
          case ClipboardIsolationStatus::ReplacementFailed:
            result.reason_code = "clipboard-test-data-unavailable";
            break;
          case ClipboardIsolationStatus::ActionSkipped:
            result.reason_code = action_reason;
            break;
          case ClipboardIsolationStatus::ActionFailed:
            result.status = ResultStatus::Fail;
            result.reason_code = action_reason;
            break;
          case ClipboardIsolationStatus::RestoreFailed:
            result.status = ResultStatus::Fail;
            result.reason_code = "clipboard-restore-failed";
            break;
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
