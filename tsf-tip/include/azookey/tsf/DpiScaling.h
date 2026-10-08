#pragma once

#include <Windows.h>

namespace azookey::tsf {

// DPI 0 means "unknown" and falls back to 96.
inline UINT NormalizeDpi(UINT dpi) { return dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi; }

// Scales a 96-DPI design value to the given DPI (copilot-pc-backend-spec §7.3).
inline int ScaleForDpi(int value, UINT dpi) {
  return MulDiv(value, static_cast<int>(NormalizeDpi(dpi)), USER_DEFAULT_SCREEN_DPI);
}

// Runs the enclosed window creation, measurement and placement as per-monitor
// aware v2 regardless of the host's awareness (copilot-pc-backend-spec §7.2).
class ScopedPerMonitorDpiAwareness {
 public:
  ScopedPerMonitorDpiAwareness()
      : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
  ~ScopedPerMonitorDpiAwareness() {
    if (previous_) SetThreadDpiAwarenessContext(previous_);
  }

  ScopedPerMonitorDpiAwareness(const ScopedPerMonitorDpiAwareness&) = delete;
  ScopedPerMonitorDpiAwareness& operator=(const ScopedPerMonitorDpiAwareness&) = delete;

 private:
  DPI_AWARENESS_CONTEXT previous_;
};

}  // namespace azookey::tsf
