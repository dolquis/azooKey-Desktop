#include <chrono>
#include <iterator>
#include <thread>

#include "runner/ClipboardIsolation.h"
#include "runner/CompatTypes.h"

namespace azookey::compat_test {

CaseDefinition MakeC011ShortcutRoutingCase() {
  return {
      "C-011",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-011";
        SystemClipboardAccess clipboard;
        std::string action_reason = "shortcut-routing-not-observed";
        const auto isolation =
            RunWithClipboardIsolation(clipboard, L"azookey-clipboard-test", [&]() {
              constexpr std::wstring_view kCopyText = L"azookey-shortcut-test";
              constexpr std::wstring_view kPasteText = L"azookey-clipboard-test";
              if (!session.ClearEditor() || !session.SendUnicode(kCopyText) ||
                  !session.SendModifiedKey({VK_CONTROL}, 'A') ||
                  !session.SendModifiedKey({VK_CONTROL}, 'C')) {
                action_reason = "control-copy-routing-not-observed";
                return false;
              }
              const auto copied = clipboard.ReadUnicodeText();
              if (!copied || *copied != kCopyText ||
                  !clipboard.ReplaceWithDeterministicText(kPasteText) ||
                  !session.SendModifiedKey({VK_CONTROL}, 'A') ||
                  !session.SendModifiedKey({VK_CONTROL}, 'V')) {
                action_reason = "control-paste-routing-not-observed";
                return false;
              }
              const auto pasted = session.ReadEditorText();
              if (!pasted || *pasted != kPasteText || !session.SendModifiedKey({VK_CONTROL}, 'L')) {
                action_reason = "control-l-routing-not-observed";
                return false;
              }
              const auto after_control_l = session.ReadEditorText();
              if (!after_control_l || *after_control_l != kPasteText ||
                  !session.SendModifiedKey({VK_CONTROL}, 'S')) {
                action_reason = "control-s-routing-not-observed";
                return false;
              }
              const auto after_control_s = session.ReadEditorText();
              if (!after_control_s || *after_control_s != kPasteText ||
                  !session.SendModifiedKey({VK_MENU}, 'F') || !session.SendVirtualKey(VK_ESCAPE)) {
                action_reason = "alt-menu-routing-not-observed";
                return false;
              }
              const auto after_alt = session.ReadEditorText();
              if (!after_alt || *after_alt != kPasteText ||
                  !session.SendModifiedKey({VK_LWIN}, 'R')) {
                action_reason = "win-shortcut-routing-not-observed";
                return false;
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(250));
              const HWND foreground = GetForegroundWindow();
              wchar_t class_name[64]{};
              if (!foreground || foreground == session.window() ||
                  GetClassNameW(foreground, class_name, static_cast<int>(std::size(class_name))) <=
                      0 ||
                  std::wstring_view(class_name) != L"#32770") {
                action_reason = "win-shortcut-routing-not-observed";
                return false;
              }
              PostMessageW(foreground, WM_CLOSE, 0, 0);
              if (!session.FocusEditor()) {
                action_reason = "focus-restore-after-win-shortcut-failed";
                return false;
              }
              return true;
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
