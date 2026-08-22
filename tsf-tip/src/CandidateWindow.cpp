#include "azookey/tsf/CandidateWindow.h"

#include <ShellScalingApi.h>

#include <algorithm>
#include <cstddef>
#include <string>

#include "CandidateSelection.h"

namespace azookey::tsf {

namespace {
constexpr wchar_t kClassName[] = L"azooKeyCandidateWnd";
constexpr wchar_t kFallbackFontFace[] = L"Yu Gothic UI";

HMODULE GetTipModuleHandle() {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&GetTipModuleHandle), &module)) {
    return module;
  }
  return nullptr;
}

UINT NormalizeDpi(UINT dpi) { return dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi; }

class ScopedThreadDpiAwarenessContext {
 public:
  explicit ScopedThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT context)
      : previous_context_(SetThreadDpiAwarenessContext(context)) {}

  ~ScopedThreadDpiAwarenessContext() {
    if (previous_context_) {
      SetThreadDpiAwarenessContext(previous_context_);
    }
  }

  ScopedThreadDpiAwarenessContext(const ScopedThreadDpiAwarenessContext&) = delete;
  ScopedThreadDpiAwarenessContext& operator=(const ScopedThreadDpiAwarenessContext&) = delete;

 private:
  DPI_AWARENESS_CONTEXT previous_context_;
};
}  // namespace

CandidateWindow::CandidateWindow() = default;

CandidateWindow::~CandidateWindow() { Destroy(); }

// static
int CandidateWindow::ScaleForDpi(int value, UINT dpi) {
  return MulDiv(value, static_cast<int>(NormalizeDpi(dpi)), static_cast<int>(kDefaultDpi));
}

// static
CandidateWindow::LayoutMetrics CandidateWindow::ComputeLayoutMetrics(UINT dpi) {
  return {
      std::max(1, ScaleForDpi(kBaseItemHeight, dpi)),
      std::max(1, ScaleForDpi(kBaseHorzPad, dpi)),
      std::max(1, ScaleForDpi(kBaseMaxWidth, dpi)),
      std::max(1, ScaleForDpi(kBaseCaretGap, dpi)),
      std::max(1, ScaleForDpi(kBaseMinTextWidth, dpi)),
      std::max(1, ScaleForDpi(kBaseExtraWidth, dpi)),
  };
}

// static
UINT CandidateWindow::DpiForMonitor(HMONITOR monitor, HWND fallback_hwnd) {
  UINT dpi_x = 0;
  UINT dpi_y = 0;
  if (monitor && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) &&
      dpi_x != 0) {
    return dpi_x;
  }
  if (fallback_hwnd) {
    UINT window_dpi = GetDpiForWindow(fallback_hwnd);
    if (window_dpi != 0) return window_dpi;
  }
  return kDefaultDpi;
}

#ifdef AZOOKEY_TSF_TESTING
// static
CandidateWindow::LayoutMetricsForTest CandidateWindow::ComputeLayoutMetricsForTest(UINT dpi) {
  const LayoutMetrics metrics = ComputeLayoutMetrics(dpi);
  return {metrics.item_height, metrics.horizontal_padding, metrics.max_width,
          metrics.caret_gap,   metrics.min_text_width,     metrics.extra_width};
}
#endif

// static
HFONT CandidateWindow::CreateMessageFont(UINT dpi) {
  dpi = NormalizeDpi(dpi);

  NONCLIENTMETRICSW nonclient_metrics{};
  nonclient_metrics.cbSize = sizeof(nonclient_metrics);
  LOGFONTW log_font{};
  if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(nonclient_metrics),
                                 &nonclient_metrics, 0, dpi)) {
    log_font = nonclient_metrics.lfMessageFont;
  } else {
    log_font.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);
    log_font.lfWeight = FW_NORMAL;
    lstrcpynW(log_font.lfFaceName, kFallbackFontFace, LF_FACESIZE);
  }
  return CreateFontIndirectW(&log_font);
}

void CandidateWindow::UpdateDpi(UINT dpi) {
  dpi = NormalizeDpi(dpi);
  if (dpi == dpi_ && font_) return;

  metrics_ = ComputeLayoutMetrics(dpi);
  HFONT next_font = CreateMessageFont(dpi);
  if (next_font) {
    if (font_) DeleteObject(font_);
    font_ = next_font;
  }
  dpi_ = dpi;
}

// static
ATOM CandidateWindow::RegisterWindowClass() {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetTipModuleHandle();
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClassName;
  ATOM a = RegisterClassExW(&wc);
  if (!a && GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
    // CreateWindowExW uses the class name, so a non-zero sentinel is enough to
    // distinguish "already registered" from real registration failure.
    a = 1;
  }
  return a;
}

bool CandidateWindow::Create() {
  static ATOM s_atom = RegisterWindowClass();
  if (!s_atom) return false;

  {
    const ScopedThreadDpiAwarenessContext dpi_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName,
                            nullptr, WS_POPUP | WS_BORDER, 0, 0, 200, metrics_.item_height, nullptr,
                            nullptr, GetTipModuleHandle(), this);
  }
  if (hwnd_) {
    UpdateDpi(GetDpiForWindow(hwnd_));
  }
  return hwnd_ != nullptr;
}

void CandidateWindow::Destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    // hwnd_ is cleared in WM_DESTROY handler.
  }
  if (font_) {
    DeleteObject(font_);
    font_ = nullptr;
  }
}

void CandidateWindow::Show(POINT pt, const std::vector<CandidateViewItem>& items,
                           int selected_idx) {
  if (!hwnd_ || items.empty()) return;

  const ScopedThreadDpiAwarenessContext dpi_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  items_ = items;
  selected_idx_ = std::clamp(selected_idx, 0, static_cast<int>(items_.size()) - 1);
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  UpdateDpi(DpiForMonitor(mon, hwnd_));

  // Measure maximum text width using the window's DC.
  HDC hdc = GetDC(hwnd_);
  HGDIOBJ old_font = nullptr;
  if (hdc && font_) old_font = SelectObject(hdc, font_);
  int max_surface_w = metrics_.min_text_width;
  int max_description_w = 0;
  if (hdc) {
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
      const auto& item = items_[static_cast<size_t>(i)];
      std::wstring label = std::to_wstring(i + 1) + L". " + item.surface;
      SIZE sz{};
      GetTextExtentPoint32W(hdc, label.c_str(), static_cast<int>(label.size()), &sz);
      max_surface_w = std::max(max_surface_w, static_cast<int>(sz.cx));
      if (!item.description.empty()) {
        GetTextExtentPoint32W(hdc, item.description.c_str(),
                              static_cast<int>(item.description.size()), &sz);
        max_description_w = std::max(max_description_w, static_cast<int>(sz.cx));
      }
    }
  }
  if (hdc) {
    if (old_font) SelectObject(hdc, old_font);
    ReleaseDC(hwnd_, hdc);
  }

  surface_column_width_ = std::min(max_surface_w, ScaleForDpi(kBaseMaxSurfaceWidth, dpi_));
  const int column_gap = max_description_w > 0 ? ScaleForDpi(kBaseColumnGap, dpi_) : 0;
  int width = std::min(surface_column_width_ + column_gap + max_description_w +
                           metrics_.horizontal_padding * 2 + metrics_.extra_width,
                       metrics_.max_width);
  int height = metrics_.item_height * static_cast<int>(items_.size());

  // Keep window on-screen: flip above caret if it would overflow below.
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  if (pt.x + width > mi.rcWork.right) pt.x = mi.rcWork.right - width;
  if (pt.x < mi.rcWork.left) pt.x = mi.rcWork.left;
  if (pt.y + height > mi.rcWork.bottom) {
    // Estimate caret height and flip to open upward when it would overflow.
    pt.y = pt.y - height - metrics_.caret_gap;
  }
  if (pt.y < mi.rcWork.top) pt.y = mi.rcWork.top;

  SetWindowPos(hwnd_, HWND_TOPMOST, pt.x, pt.y, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
  InvalidateRect(hwnd_, nullptr, TRUE);
}

void CandidateWindow::Hide() {
  if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void CandidateWindow::PostCandidatesReady() {
  if (hwnd_) PostMessageW(hwnd_, kCandidatesReadyMessage, 0, 0);
}

bool CandidateWindow::IsVisible() const { return hwnd_ && IsWindowVisible(hwnd_); }

void CandidateWindow::MoveSelection(int delta) {
  if (items_.empty()) return;
  int n = static_cast<int>(items_.size());
  selected_idx_ = internal::WrapCandidateSelectionIndex(selected_idx_, delta, n);
  Repaint();
}

void CandidateWindow::SetSelected(int idx) {
  if (items_.empty()) return;
  selected_idx_ = std::clamp(idx, 0, static_cast<int>(items_.size()) - 1);
  Repaint();
}

void CandidateWindow::Repaint() const {
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// static
LRESULT CALLBACK CandidateWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  CandidateWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
    self = static_cast<CandidateWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self) return self->HandleMessage(hwnd, msg, wParam, lParam);
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CandidateWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      UpdateDpi(GetDpiForWindow(hwnd));
      HGDIOBJ old_font = nullptr;
      if (font_) old_font = SelectObject(hdc, font_);
      SetBkMode(hdc, TRANSPARENT);

      RECT client_rc{};
      GetClientRect(hwnd, &client_rc);

      for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        RECT row_rc = {0, i * metrics_.item_height, client_rc.right,
                       (i + 1) * metrics_.item_height};
        if (i == selected_idx_) {
          FillRect(hdc, &row_rc,
                   reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_HIGHLIGHT + 1)));
          SetTextColor(hdc, GetSysColor(COLOR_HIGHLIGHTTEXT));
        } else {
          FillRect(hdc, &row_rc, reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1)));
          SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        }
        const auto& item = items_[static_cast<size_t>(i)];
        std::wstring label = std::to_wstring(i + 1) + L". " + item.surface;
        RECT surface_rc = row_rc;
        surface_rc.left += metrics_.horizontal_padding;
        surface_rc.right = std::min(surface_rc.right - metrics_.horizontal_padding,
                                    surface_rc.left + surface_column_width_);
        DrawTextW(hdc, label.c_str(), static_cast<int>(label.size()), &surface_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (!item.description.empty()) {
          if (i != selected_idx_) SetTextColor(hdc, RGB(96, 96, 96));
          RECT description_rc = row_rc;
          description_rc.left = surface_rc.right + ScaleForDpi(kBaseColumnGap, dpi_);
          description_rc.right -= metrics_.horizontal_padding;
          if (description_rc.left < description_rc.right) {
            DrawTextW(hdc, item.description.c_str(), static_cast<int>(item.description.size()),
                      &description_rc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
          }
        }
      }
      if (old_font) SelectObject(hdc, old_font);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_LBUTTONDOWN: {
      int y = static_cast<int>(HIWORD(lParam));
      int idx = y / metrics_.item_height;
      if (idx >= 0 && idx < static_cast<int>(items_.size())) {
        selected_idx_ = idx;
        Repaint();
        if (on_click_) on_click_(idx);
      }
      return 0;
    }

    case kCandidatesReadyMessage:
      if (on_candidates_ready_) on_candidates_ready_(on_candidates_ready_context_);
      return 0;

    case WM_DPICHANGED: {
      UpdateDpi(LOWORD(wParam));
      RECT* suggested = reinterpret_cast<RECT*>(lParam);
      if (suggested) {
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      }
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }

    case WM_DESTROY:
      hwnd_ = nullptr;
      return 0;

    default:
      // Use the hwnd parameter from WndProc, not the hwnd_ member: after
      // WM_DESTROY sets hwnd_ to nullptr, trailing messages (e.g. WM_NCDESTROY)
      // would otherwise forward with a null handle.
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

}  // namespace azookey::tsf
