#include "azookey/tsf/PredictionWindow.h"

#include <ShellScalingApi.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cassert>
#include <cmath>

#include "azookey/tsf/CaretRectResolver.h"
#include "azookey/tsf/DpiScaling.h"
#include "azookey/tsf/RenderingEngine.h"

namespace azookey::tsf {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"azooKeyPredictionWindow";

int Scale(int value, UINT dpi) { return ScaleForDpi(value, dpi); }

HMODULE TipModule() {
  HMODULE module = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&TipModule), &module);
  return module;
}

D2D1_COLOR_F ToColorF(COLORREF color) {
  return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                      GetBValue(color) / 255.0f, 1.0f);
}

#ifdef AZOOKEY_TSF_TESTING
UINT g_monitor_dpi_for_test = 0;
#endif

// The GPU was reset or removed; the whole device stack has to be rebuilt.
bool IsDeviceLost(HRESULT hr) {
  return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
         hr == D2DERR_RECREATE_TARGET;
}

}  // namespace

#ifdef AZOOKEY_TSF_TESTING
void PredictionWindow::SetMonitorDpiForTest(UINT dpi) { g_monitor_dpi_for_test = dpi; }
bool PredictionWindow::IsDeviceLostForTest(HRESULT hr) { return IsDeviceLost(hr); }
#endif

struct PredictionWindow::RenderState {
  RenderingEngine engine;
  ComPtr<IDWriteTextFormat> text_format;
};

PredictionWindow::PredictionWindow() = default;
PredictionWindow::~PredictionWindow() { Destroy(); }

RECT PredictionWindow::ComputePlacement(RECT caret, RECT work_area, int width, int height) {
  width = std::max(width, 0);
  height = std::max(height, 0);
  const int min_x = work_area.left;
  const int max_x = std::max(min_x, static_cast<int>(work_area.right) - width);
  const int min_y = work_area.top;
  const int max_y = std::max(min_y, static_cast<int>(work_area.bottom) - height);

  int x = caret.right + 4;
  if (x + width > work_area.right) x = caret.left - width - 4;
  int y = caret.top;
  if (y + height > work_area.bottom) y = caret.bottom - height;
  x = std::clamp(x, min_x, max_x);
  y = std::clamp(y, min_y, max_y);
  return {x, y, x + width, y + height};
}

ATOM PredictionWindow::RegisterWindowClass() {
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WndProc;
  window_class.hInstance = TipModule();
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  const ATOM atom = RegisterClassExW(&window_class);
  return atom ? atom : (GetLastError() == ERROR_CLASS_ALREADY_EXISTS ? 1 : 0);
}

bool PredictionWindow::InitializeRendering() {
  auto state = std::make_unique<RenderState>();
  if (!state->engine.Initialize(hwnd_, SurfaceAlpha::Opaque))
    return Fail(state->engine.failure_stage(), state->engine.failure_hr());
  render_ = std::move(state);
  UpdateDpi(GetDpiForWindow(hwnd_));
  UpdateTheme();
  return render_->text_format != nullptr || Fail("text_format", E_FAIL);
}

bool PredictionWindow::Fail(const char* stage, HRESULT hr) {
  failure_stage_ = stage;
  failure_hr_ = hr;
  // Drop a lost device; the next Show initializes a new one.
  if (IsDeviceLost(hr)) render_.reset();
  return false;
}

bool PredictionWindow::Create() {
  try {
    if (hwnd_) return GetCurrentThreadId() == ui_thread_id_ || Fail("wrong_thread", E_FAIL);
    failure_stage_ = "";
    failure_hr_ = S_OK;
    static const ATOM atom = RegisterWindowClass();
    if (!atom) return Fail("register_class", E_FAIL);

    const ScopedPerMonitorDpiAwareness dpi_context;
    ui_thread_id_ = GetCurrentThreadId();
    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        kWindowClass, nullptr, WS_POPUP | WS_CLIPSIBLINGS, 0, 0, 1, 1, nullptr, nullptr,
        TipModule(), this);
    if (!hwnd_) return Fail("create_window", HRESULT_FROM_WIN32(GetLastError()));
    if (InitializeRendering()) return true;
  } catch (...) {
    // Allocation failure while setting up DirectComposition is a creation failure.
    Fail("exception", E_OUTOFMEMORY);
  }
  Destroy();
  return false;
}

void PredictionWindow::Destroy() {
  if (hwnd_) {
    assert(GetCurrentThreadId() == ui_thread_id_);
    DestroyWindow(hwnd_);
  }
  render_.reset();
  candidates_.clear();
  width_ = 0;
  height_ = 0;
}

void PredictionWindow::UpdateDpi(UINT dpi) {
  dpi_ = dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
  row_height_ = Scale(28, dpi_);
  padding_ = Scale(8, dpi_);
  if (!render_) return;

  ComPtr<IDWriteTextFormat> format;
  const FLOAT size = 14.0f * static_cast<FLOAT>(dpi_) / USER_DEFAULT_SCREEN_DPI;
  for (const wchar_t* family : {L"Yu Gothic UI", L"Meiryo UI", L"MS UI Gothic"}) {
    if (SUCCEEDED(render_->engine.write_factory()->CreateTextFormat(
            family, nullptr, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"ja-JP", &format)))
      break;
  }
  if (format) {
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    render_->text_format = std::move(format);
  }
}

int PredictionWindow::MeasureWidth() const {
  if (!render_ || !render_->text_format) return Scale(200, dpi_);
  FLOAT widest = 0;
  for (const auto& text : candidates_) {
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(render_->engine.write_factory()->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), render_->text_format.Get(), 4096.0f,
            static_cast<FLOAT>(row_height_), &layout))) {
      DWRITE_TEXT_METRICS metrics{};
      if (SUCCEEDED(layout->GetMetrics(&metrics))) widest = std::max(widest, metrics.width);
    }
  }
  return std::clamp(static_cast<int>(std::ceil(widest)) + padding_ * 2, Scale(160, dpi_),
                    Scale(420, dpi_));
}

bool PredictionWindow::ResizeSurface(int width, int height) {
  if (!render_) return Fail("no_render", E_FAIL);
  return render_->engine.ResizeSurface(width, height) ||
         Fail(render_->engine.failure_stage(), render_->engine.failure_hr());
}

void PredictionWindow::UpdateTheme() {
  theme_mode_ = CurrentThemeMode();
  theme_ = ResolveThemeColors(theme_mode_);
  ApplyWindowFrameTheme(hwnd_, theme_mode_);
}

bool PredictionWindow::Draw() {
  if (!render_) return Fail("no_render", E_FAIL);
  ID2D1DeviceContext* context = render_->engine.BeginDraw();
  if (!context) return Fail(render_->engine.failure_stage(), render_->engine.failure_hr());

  context->Clear(ToColorF(theme_.background));
  ComPtr<ID2D1SolidColorBrush> text_brush;
  ComPtr<ID2D1SolidColorBrush> border_brush;
  bool drawn = SUCCEEDED(context->CreateSolidColorBrush(ToColorF(theme_.text), &text_brush)) &&
               SUCCEEDED(context->CreateSolidColorBrush(ToColorF(theme_.border), &border_brush));
  if (drawn) {
    context->DrawRectangle(D2D1::RectF(0.5f, 0.5f, static_cast<FLOAT>(width_) - 0.5f,
                                       static_cast<FLOAT>(height_) - 0.5f),
                           border_brush.Get());
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
      const FLOAT y = static_cast<FLOAT>(padding_ + static_cast<int>(index) * row_height_);
      const auto& text = candidates_[index];
      context->DrawText(
          text.c_str(), static_cast<UINT32>(text.size()), render_->text_format.Get(),
          D2D1::RectF(static_cast<FLOAT>(padding_), y, static_cast<FLOAT>(width_ - padding_),
                      y + static_cast<FLOAT>(row_height_)),
          text_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
  }

  const bool ended = render_->engine.EndDraw();
  if (!drawn) return Fail("brush", E_FAIL);
  return ended || Fail(render_->engine.failure_stage(), render_->engine.failure_hr());
}

void PredictionWindow::Show(const std::vector<std::wstring>& candidates,
                            RECT caret_rect_screen) try {
  if (!hwnd_) return;
  assert(GetCurrentThreadId() == ui_thread_id_);
  failure_stage_ = "";
  failure_hr_ = S_OK;
  const ScopedPerMonitorDpiAwareness dpi_context;
  struct ShowingScope {
    bool& flag;
    explicit ShowingScope(bool& showing) : flag(showing) { flag = true; }
    ~ShowingScope() { flag = false; }
    ShowingScope(const ShowingScope&) = delete;
    ShowingScope& operator=(const ShowingScope&) = delete;
  } showing(showing_);
  last_caret_rect_ = caret_rect_screen;
  if (candidates.empty()) {
    Hide();
    return;
  }
  if (!render_ && !InitializeRendering()) {
    Hide();
    return;
  }

  candidates_.assign(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(
                                                                  VisibleCount(candidates.size())));
  const POINT center{
      caret_rect_screen.left + (caret_rect_screen.right - caret_rect_screen.left) / 2,
      caret_rect_screen.top + (caret_rect_screen.bottom - caret_rect_screen.top) / 2};
  const MonitorWorkArea monitor = ResolveMonitorWorkArea(DefaultMonitorWin32Api(), center);
  UINT dpi_x = 0;
  UINT dpi_y = 0;
  if (!monitor.monitor ||
      FAILED(GetDpiForMonitor(monitor.monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) || !dpi_x)
    dpi_x = GetDpiForWindow(hwnd_);
#ifdef AZOOKEY_TSF_TESTING
  if (g_monitor_dpi_for_test) dpi_x = g_monitor_dpi_for_test;
#endif
  if (dpi_x != dpi_) UpdateDpi(dpi_x);

  width_ = MeasureWidth();
  height_ = padding_ * 2 + row_height_ * static_cast<int>(candidates_.size());
  RECT work_area = monitor.work_area;
  if (work_area.right > work_area.left && work_area.bottom > work_area.top) {
    width_ = std::min(width_, static_cast<int>(work_area.right - work_area.left));
    height_ = std::min(height_, static_cast<int>(work_area.bottom - work_area.top));
  }
  if (width_ <= 0 || height_ <= 0) {
    Fail("size", E_FAIL);
    return;
  }
  const RECT placement = ComputePlacement(caret_rect_screen, work_area, width_, height_);
  if (!ResizeSurface(width_, height_)) {
    Hide();
    return;
  }
  if (!SetWindowPos(hwnd_, HWND_TOPMOST, placement.left, placement.top, width_, height_,
                    SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
    Fail("set_window_pos", HRESULT_FROM_WIN32(GetLastError()));
    Hide();
    return;
  }
  if (Draw()) {
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    if (!IsWindowVisible(hwnd_)) Fail("show_window", E_FAIL);
  } else {
    Hide();
  }
} catch (...) {
  Fail("exception", E_OUTOFMEMORY);
  Hide();
}

void PredictionWindow::Hide() {
  if (!hwnd_) return;
  assert(GetCurrentThreadId() == ui_thread_id_);
  ShowWindow(hwnd_, SW_HIDE);
  candidates_.clear();
}

bool PredictionWindow::IsVisible() const { return hwnd_ && IsWindowVisible(hwnd_); }

LRESULT CALLBACK PredictionWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  PredictionWindow* self = nullptr;
  if (message == WM_NCCREATE) {
    self = static_cast<PredictionWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<PredictionWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
  try {
    return self->HandleMessage(hwnd, message, wparam, lparam);
  } catch (...) {
    // Never unwind through the Win32 window procedure.
    return message == WM_NCCREATE ? FALSE : 0;
  }
}

LRESULT PredictionWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_LBUTTONUP: {
      const int y = GET_Y_LPARAM(lparam) - padding_;
      if (y >= 0 && y < row_height_ * static_cast<int>(candidates_.size()) && on_click_) {
        try {
          const auto callback = on_click_;
          callback(y / row_height_);
        } catch (...) {
          // A client callback must not unwind through the Win32 window procedure.
        }
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(hwnd, &paint);
      EndPaint(hwnd, &paint);
      return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      if (message != WM_SETTINGCHANGE || IsThemeSettingChange(wparam, lparam)) {
        UpdateTheme();
        if (IsVisible() && !Draw()) Hide();
      }
      return 0;
    case WM_DPICHANGED:
      // The suggested rect is ignored: the surface size and the work-area
      // placement both come from Show, which measures for the caret's monitor.
      if (showing_) return 0;
      UpdateDpi(LOWORD(wparam));
      if (IsVisible() && !candidates_.empty()) {
        const std::vector<std::wstring> candidates = candidates_;
        Show(candidates, last_caret_rect_);
      }
      return 0;
    case WM_NCDESTROY:
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      hwnd_ = nullptr;
      break;
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace azookey::tsf
