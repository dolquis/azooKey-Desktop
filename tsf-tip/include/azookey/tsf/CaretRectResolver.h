#pragma once

#include <Windows.h>

namespace azookey::tsf {

// Win32 entry points used to resolve the caret (legacy-parity-spec §9).
// Tests substitute fakes; production code uses DefaultCaretWin32Api().
struct CaretWin32Api {
  BOOL(WINAPI* get_gui_thread_info)(DWORD, PGUITHREADINFO);
  BOOL(WINAPI* client_to_screen)(HWND, LPPOINT);
  BOOL(WINAPI* get_physical_cursor_pos)(LPPOINT);
  BOOL(WINAPI* logical_to_physical_point)(HWND, LPPOINT);
  UINT (*get_monitor_scale_percent)(POINT);
};

CaretWin32Api DefaultCaretWin32Api();

struct CaretAnchor {
  POINT point{0, 0};
  bool valid{false};
};

bool IsUsableTextExtent(const RECT& rect);

// Bottom-left of the caret in physical screen coordinates. Tries the text
// extent, then GetGUIThreadInfo's rcCaret, then the physical cursor position.
CaretAnchor ResolveCaretAnchor(const CaretWin32Api& api, const RECT* text_ext_rect,
                               HWND text_extent_window);

// Converts a usable text extent to physical screen coordinates. Returns false
// when a window is given and either corner fails to convert.
bool ResolveCaretRect(const CaretWin32Api& api, const RECT& text_ext_rect, HWND text_extent_window,
                      RECT* physical_rect);

// A 1-pixel-wide caret ending at the anchor, using the fallback caret height
// scaled for the anchor's monitor.
RECT CaretRectFromAnchor(const CaretWin32Api& api, POINT anchor);

// Win32 entry points used to find the work area a popup must stay within.
struct MonitorWin32Api {
  HMONITOR(WINAPI* monitor_from_point)(POINT, DWORD);
  BOOL(WINAPI* get_monitor_info)(HMONITOR, LPMONITORINFO);
  int(WINAPI* get_system_metrics)(int);
};

MonitorWin32Api DefaultMonitorWin32Api();

struct MonitorWorkArea {
  HMONITOR monitor{nullptr};
  RECT work_area{0, 0, 0, 0};
};

// Work area of the monitor nearest to the point. Falls back to the virtual
// screen when the monitor information is unavailable.
MonitorWorkArea ResolveMonitorWorkArea(const MonitorWin32Api& api, POINT point);

}  // namespace azookey::tsf
