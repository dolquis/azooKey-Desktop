#include <chrono>
#include <optional>
#include <thread>
#include <vector>

#include "runner/CaseSupport.h"
#include "runner/CompatTypes.h"

namespace azookey::compat_test {
namespace {

struct DpiMonitor {
  HMONITOR handle{};
  MONITORINFO info{};
};

std::optional<DpiMonitor> MoveTo150PercentMonitor(HWND window) {
  std::vector<HMONITOR> monitors;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM value) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(value)->push_back(monitor);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&monitors));
  for (const HMONITOR monitor : monitors) {
    DpiMonitor target;
    target.handle = monitor;
    target.info.cbSize = sizeof(target.info);
    if (!GetMonitorInfoW(monitor, &target.info)) continue;
    const int width = 640;
    const int height = 480;
    const int x =
        target.info.rcWork.left + (target.info.rcWork.right - target.info.rcWork.left - width) / 2;
    const int y =
        target.info.rcWork.top + (target.info.rcWork.bottom - target.info.rcWork.top - height) / 2;
    if (!SetWindowPos(window, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER)) {
      continue;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (GetDpiForWindow(window) == 144) return target;
  }
  return std::nullopt;
}

}  // namespace

CaseDefinition MakeC006DpiScalingCase() {
  return {
      "C-006",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-006";
        result.status = ResultStatus::FailingSkip;
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        WINDOWPLACEMENT original{};
        original.length = sizeof(original);
        if (!GetWindowPlacement(session.window(), &original)) {
          result.reason_code = "window-placement-unavailable";
          return result;
        }
        const auto monitor = MoveTo150PercentMonitor(session.window());
        if (!monitor) {
          SetWindowPlacement(session.window(), &original);
          result.reason_code = "dpi-150-monitor-unavailable";
          return result;
        }
        if (!session.FocusEditor() || !session.ClearEditor() || !session.SendAscii("nihongo") ||
            !session.SendVirtualKey(VK_SPACE)) {
          SetWindowPlacement(session.window(), &original);
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
        SetWindowPlacement(session.window(), &original);
        session.FocusEditor();
        if (!candidate) {
          result.status = ResultStatus::Fail;
          result.reason_code = "candidate-window-not-found";
        } else if (!caret || dpi != 144) {
          result.reason_code = "dpi-geometry-unavailable";
        } else if (IsCandidateNearCaretAtDpi(*candidate, *caret, dpi) &&
                   IsRectWithinBounds(*candidate, monitor->info.rcMonitor)) {
          result.status = ResultStatus::Pass;
          result.reason_code = "candidate-scaled-at-150-percent";
        } else {
          result.status = ResultStatus::Fail;
          result.reason_code = "candidate-dpi-position-out-of-range";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
