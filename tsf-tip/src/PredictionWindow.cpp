#include "azookey/tsf/PredictionWindow.h"

#include <ShellScalingApi.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cassert>
#include <cmath>

namespace azookey::tsf {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"azooKeyPredictionWindow";

int Scale(int value, UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI),
                USER_DEFAULT_SCREEN_DPI);
}

HMODULE TipModule() {
  HMODULE module = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&TipModule), &module);
  return module;
}

D2D1_COLOR_F SystemColor(int color_index) {
  const COLORREF color = GetSysColor(color_index);
  return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                      GetBValue(color) / 255.0f, 1.0f);
}

class ScopedThreadDpiAwarenessContext {
 public:
  ScopedThreadDpiAwarenessContext()
      : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
  ~ScopedThreadDpiAwarenessContext() {
    if (previous_) SetThreadDpiAwarenessContext(previous_);
  }

 private:
  DPI_AWARENESS_CONTEXT previous_;
};

}  // namespace

struct PredictionWindow::RenderState {
  ComPtr<ID3D11Device> d3d_device;
  ComPtr<ID2D1Factory1> d2d_factory;
  ComPtr<ID2D1Device> d2d_device;
  ComPtr<IDCompositionDevice2> composition_device;
  ComPtr<IDCompositionDesktopDevice> desktop_device;
  ComPtr<IDCompositionTarget> target;
  ComPtr<IDCompositionVisual2> visual;
  ComPtr<IDCompositionSurface> surface;
  ComPtr<IDWriteFactory> write_factory;
  ComPtr<IDWriteTextFormat> text_format;
  int surface_width{0};
  int surface_height{0};
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
  constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                 D3D11_SDK_VERSION, &state->d3d_device, nullptr, nullptr);
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                           D3D11_SDK_VERSION, &state->d3d_device, nullptr, nullptr);
  }
  if (FAILED(hr)) return false;

  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(state->d3d_device.As(&dxgi_device))) return false;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                               reinterpret_cast<void**>(state->d2d_factory.GetAddressOf()))))
    return false;
  if (FAILED(state->d2d_factory->CreateDevice(dxgi_device.Get(), &state->d2d_device))) return false;

  // A Direct2D device is required here so BeginDraw can return a device context.
  if (FAILED(DCompositionCreateDevice2(
          state->d2d_device.Get(), __uuidof(IDCompositionDevice2),
          reinterpret_cast<void**>(state->composition_device.GetAddressOf()))))
    return false;
  if (FAILED(state->composition_device.As(&state->desktop_device)) ||
      FAILED(state->desktop_device->CreateTargetForHwnd(hwnd_, TRUE, &state->target)) ||
      FAILED(state->composition_device->CreateVisual(&state->visual)) ||
      FAILED(state->target->SetRoot(state->visual.Get())))
    return false;

  if (FAILED(
          DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(state->write_factory.GetAddressOf()))))
    return false;

  render_ = std::move(state);
  UpdateDpi(GetDpiForWindow(hwnd_));
  return render_->text_format != nullptr;
}

bool PredictionWindow::Create() {
  try {
    if (hwnd_) return GetCurrentThreadId() == ui_thread_id_;
    static const ATOM atom = RegisterWindowClass();
    if (!atom) return false;

    const ScopedThreadDpiAwarenessContext dpi_context;
    ui_thread_id_ = GetCurrentThreadId();
    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        kWindowClass, nullptr, WS_POPUP | WS_CLIPSIBLINGS, 0, 0, 1, 1, nullptr, nullptr,
        TipModule(), this);
    if (!hwnd_) return false;
    if (InitializeRendering()) return true;
  } catch (...) {
    // Allocation failure while setting up DirectComposition is a creation failure.
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
    if (SUCCEEDED(render_->write_factory->CreateTextFormat(
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
    if (SUCCEEDED(render_->write_factory->CreateTextLayout(
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
  if (!render_) return false;
  if (render_->surface && render_->surface_width == width && render_->surface_height == height)
    return true;

  ComPtr<IDCompositionSurface> surface;
  if (FAILED(render_->composition_device->CreateSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                        DXGI_ALPHA_MODE_PREMULTIPLIED, &surface)) ||
      FAILED(render_->visual->SetContent(surface.Get())))
    return false;
  render_->surface = std::move(surface);
  render_->surface_width = width;
  render_->surface_height = height;
  return true;
}

bool PredictionWindow::Draw() {
  if (!render_ || !render_->surface) return false;

  ComPtr<ID2D1DeviceContext> context;
  POINT offset{};
  if (FAILED(render_->surface->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
                                         reinterpret_cast<void**>(context.GetAddressOf()),
                                         &offset)))
    return false;

  context->SetDpi(96.0f, 96.0f);
  context->SetTransform(
      D2D1::Matrix3x2F::Translation(static_cast<FLOAT>(offset.x), static_cast<FLOAT>(offset.y)));
  context->Clear(SystemColor(COLOR_WINDOW));

  ComPtr<ID2D1SolidColorBrush> text_brush;
  ComPtr<ID2D1SolidColorBrush> border_brush;
  bool drawn =
      SUCCEEDED(context->CreateSolidColorBrush(SystemColor(COLOR_WINDOWTEXT), &text_brush)) &&
      SUCCEEDED(context->CreateSolidColorBrush(SystemColor(COLOR_3DSHADOW), &border_brush));
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

  const HRESULT end_draw = render_->surface->EndDraw();
  context.Reset();
  return drawn && SUCCEEDED(end_draw) && SUCCEEDED(render_->composition_device->Commit());
}

void PredictionWindow::Show(const std::vector<std::wstring>& candidates,
                            RECT caret_rect_screen) try {
  if (!hwnd_) return;
  assert(GetCurrentThreadId() == ui_thread_id_);
  const ScopedThreadDpiAwarenessContext dpi_context;
  if (candidates.empty()) {
    Hide();
    return;
  }

  candidates_.assign(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(
                                                                  VisibleCount(candidates.size())));
  const POINT center{
      caret_rect_screen.left + (caret_rect_screen.right - caret_rect_screen.left) / 2,
      caret_rect_screen.top + (caret_rect_screen.bottom - caret_rect_screen.top) / 2};
  const HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
  UINT dpi_x = 0;
  UINT dpi_y = 0;
  if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) || !dpi_x)
    dpi_x = GetDpiForWindow(hwnd_);
  if (dpi_x != dpi_) UpdateDpi(dpi_x);

  width_ = MeasureWidth();
  height_ = padding_ * 2 + row_height_ * static_cast<int>(candidates_.size());
  MONITORINFO info{sizeof(info)};
  RECT work_area = caret_rect_screen;
  if (GetMonitorInfoW(monitor, &info)) work_area = info.rcWork;
  if (work_area.right > work_area.left && work_area.bottom > work_area.top) {
    width_ = std::min(width_, static_cast<int>(work_area.right - work_area.left));
    height_ = std::min(height_, static_cast<int>(work_area.bottom - work_area.top));
  }
  if (width_ <= 0 || height_ <= 0) return;
  const RECT placement = ComputePlacement(caret_rect_screen, work_area, width_, height_);
  if (!ResizeSurface(width_, height_)) {
    Hide();
    return;
  }
  if (!SetWindowPos(hwnd_, HWND_TOPMOST, placement.left, placement.top, width_, height_,
                    SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
    Hide();
    return;
  }
  if (Draw())
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  else
    Hide();
} catch (...) {
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
      if (IsVisible()) Draw();
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
