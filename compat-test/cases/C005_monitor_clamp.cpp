#include <chrono>
#include <optional>
#include <thread>
#include <vector>

#include "runner/CaseSupport.h"
#include "runner/CompatTypes.h"

namespace azookey::compat_test {
namespace {

struct MonitorTarget {
  HMONITOR handle{};
  MONITORINFO info{};
};

std::optional<MonitorTarget> SecondaryMonitor() {
  std::vector<HMONITOR> monitors;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM value) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(value)->push_back(monitor);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&monitors));
  for (const HMONITOR monitor : monitors) {
    MonitorTarget target;
    target.handle = monitor;
    target.info.cbSize = sizeof(target.info);
    if (GetMonitorInfoW(monitor, &target.info) &&
        (target.info.dwFlags & MONITORINFOF_PRIMARY) == 0) {
      return target;
    }
  }
  return std::nullopt;
}

class WindowPlacementGuard {
 public:
  explicit WindowPlacementGuard(HWND window) : window_(window) {
    placement_.length = sizeof(placement_);
    valid_ = GetWindowPlacement(window_, &placement_) != FALSE;
  }
  ~WindowPlacementGuard() {
    if (valid_ && IsWindow(window_)) SetWindowPlacement(window_, &placement_);
  }
  bool valid() const { return valid_; }

 private:
  HWND window_{};
  WINDOWPLACEMENT placement_{};
  bool valid_{false};
};

}  // namespace

CaseDefinition MakeC005MonitorClampCase() {
  return {
      "C-005",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-005";
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        const auto monitor = SecondaryMonitor();
        if (!monitor) {
          result.reason_code = "secondary-monitor-unavailable";
          return result;
        }
        WindowPlacementGuard restore(session.window());
        if (!restore.valid()) {
          result.reason_code = "window-placement-unavailable";
          return result;
        }
        constexpr int kWidth = 420;
        constexpr int kHeight = 320;
        const int x = monitor->info.rcWork.right - kWidth;
        const int y = monitor->info.rcWork.top + 80;
        if (!SetWindowPos(session.window(), nullptr, x, y, kWidth, kHeight,
                          SWP_NOACTIVATE | SWP_NOZORDER) ||
            !session.FocusEditor() || !session.ClearEditor() ||
            !session.SendUnicode(L"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx") ||
            !session.SendAscii("nihongo") || !session.SendVirtualKey(VK_SPACE)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        std::optional<RECT> candidate;
        for (int attempt = 0; attempt < 30 && !candidate; ++attempt) {
          candidate = session.CandidateRect();
          if (!candidate) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const auto caret = session.CaretRect();
        const UINT dpi = GetDpiForWindow(session.window());
        const LONG edge_margin = MulDiv(160, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
        if (!caret || dpi == 0 || caret->left < monitor->info.rcMonitor.right - edge_margin) {
          result.reason_code = "monitor-edge-precondition-not-observed";
        } else if (!candidate) {
          result.status = ResultStatus::Fail;
          result.reason_code = "candidate-window-not-found";
        } else if (IsRectWithinBounds(*candidate, monitor->info.rcMonitor)) {
          result.status = ResultStatus::Pass;
          result.reason_code = "candidate-clamped-to-monitor";
        } else {
          result.status = ResultStatus::Fail;
          result.reason_code = "candidate-outside-monitor";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
