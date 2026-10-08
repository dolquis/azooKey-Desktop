#include "azookey/tsf/CaretRectResolver.h"

#include <shellscalingapi.h>

namespace azookey::tsf {
namespace {

constexpr UINT kDefaultMonitorScalePercent = 100;
// legacy-parity-spec §9.1: the cursor fallback assumes a 16 logical px caret.
constexpr LONG kFallbackCaretHeight = 16;

UINT GetMonitorScalePercent(POINT screen_point) {
  const HMONITOR monitor = MonitorFromPoint(screen_point, MONITOR_DEFAULTTONEAREST);
  if (!monitor) return kDefaultMonitorScalePercent;

  DEVICE_SCALE_FACTOR scale_factor = SCALE_100_PERCENT;
  if (FAILED(GetScaleFactorForMonitor(monitor, &scale_factor))) {
    return kDefaultMonitorScalePercent;
  }
  return static_cast<UINT>(scale_factor);
}

bool IsZeroRect(const RECT& rect) {
  return rect.left == 0 && rect.top == 0 && rect.right == 0 && rect.bottom == 0;
}

LONG FallbackCaretHeight(const CaretWin32Api& api, POINT point) {
  const UINT scale_percent = api.get_monitor_scale_percent ? api.get_monitor_scale_percent(point)
                                                           : kDefaultMonitorScalePercent;
  return scale_percent > 0 ? MulDiv(kFallbackCaretHeight, static_cast<int>(scale_percent), 100)
                           : kFallbackCaretHeight;
}

bool IsEmptyRect(const RECT& rect) { return rect.right <= rect.left || rect.bottom <= rect.top; }

}  // namespace

CaretWin32Api DefaultCaretWin32Api() {
  return {&::GetGUIThreadInfo, &::ClientToScreen, &::GetPhysicalCursorPos,
          &::LogicalToPhysicalPointForPerMonitorDPI, &GetMonitorScalePercent};
}

bool IsUsableTextExtent(const RECT& rect) {
  return rect.right > rect.left && rect.bottom > rect.top;
}

CaretAnchor ResolveCaretAnchor(const CaretWin32Api& api, const RECT* text_ext_rect,
                               HWND text_extent_window) {
  if (text_ext_rect && IsUsableTextExtent(*text_ext_rect)) {
    POINT point{text_ext_rect->left, text_ext_rect->bottom};
    if (text_extent_window && api.logical_to_physical_point) {
      POINT physical_point = point;
      if (api.logical_to_physical_point(text_extent_window, &physical_point)) {
        point = physical_point;
      }
    }
    return {point, true};
  }

  GUITHREADINFO thread_info{};
  thread_info.cbSize = sizeof(thread_info);
  if (api.get_gui_thread_info && api.client_to_screen && api.get_gui_thread_info(0, &thread_info) &&
      thread_info.hwndCaret && !IsZeroRect(thread_info.rcCaret)) {
    POINT point{thread_info.rcCaret.left, thread_info.rcCaret.bottom};
    if (api.client_to_screen(thread_info.hwndCaret, &point)) {
      if (api.logical_to_physical_point) {
        POINT physical_point = point;
        if (api.logical_to_physical_point(thread_info.hwndCaret, &physical_point)) {
          point = physical_point;
        }
      }
      return {point, true};
    }
  }

  POINT point{};
  if (api.get_physical_cursor_pos && api.get_physical_cursor_pos(&point)) {
    point.y += FallbackCaretHeight(api, point);
    return {point, true};
  }
  return {};
}

bool ResolveCaretRect(const CaretWin32Api& api, const RECT& text_ext_rect, HWND text_extent_window,
                      RECT* physical_rect) {
  if (!physical_rect || !IsUsableTextExtent(text_ext_rect)) return false;
  if (!text_extent_window) {
    *physical_rect = text_ext_rect;
    return true;
  }
  POINT top_left{text_ext_rect.left, text_ext_rect.top};
  POINT bottom_right{text_ext_rect.right, text_ext_rect.bottom};
  if (!api.logical_to_physical_point ||
      !api.logical_to_physical_point(text_extent_window, &top_left) ||
      !api.logical_to_physical_point(text_extent_window, &bottom_right)) {
    return false;
  }
  *physical_rect = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
  return true;
}

RECT CaretRectFromAnchor(const CaretWin32Api& api, POINT anchor) {
  return {anchor.x, anchor.y - FallbackCaretHeight(api, anchor), anchor.x + 1, anchor.y};
}

MonitorWin32Api DefaultMonitorWin32Api() {
  return {&::MonitorFromPoint, &::GetMonitorInfoW, &::GetSystemMetrics};
}

MonitorWorkArea ResolveMonitorWorkArea(const MonitorWin32Api& api, POINT point) {
  MonitorWorkArea result;
  result.monitor =
      api.monitor_from_point ? api.monitor_from_point(point, MONITOR_DEFAULTTONEAREST) : nullptr;
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (result.monitor && api.get_monitor_info && api.get_monitor_info(result.monitor, &info) &&
      !IsEmptyRect(info.rcWork)) {
    result.work_area = info.rcWork;
    return result;
  }
  if (api.get_system_metrics) {
    const int left = api.get_system_metrics(SM_XVIRTUALSCREEN);
    const int top = api.get_system_metrics(SM_YVIRTUALSCREEN);
    const RECT virtual_screen{left, top, left + api.get_system_metrics(SM_CXVIRTUALSCREEN),
                              top + api.get_system_metrics(SM_CYVIRTUALSCREEN)};
    if (!IsEmptyRect(virtual_screen)) result.work_area = virtual_screen;
  }
  return result;
}

}  // namespace azookey::tsf
