#include "azookey/tsf/DebugWindow.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "azookey/core/BodyLogGate.h"
#include "azookey/tsf/RenderingEngine.h"

namespace azookey::tsf {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kDebugWindowClass[] = L"azooKeyDebugWindow";
constexpr int kDebugWindowWidth = 600;
constexpr int kDebugWindowHeight = 400;
constexpr int kDebugWindowMargin = 16;
constexpr int kDebugWindowPadding = 6;
constexpr BYTE kDebugWindowAlpha = 220;

HMODULE DebugWindowModule() {
  HMODULE module = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&DebugWindowModule), &module);
  return module;
}

std::wstring DebugUtf8ToWide(const std::string& text) {
  if (text.empty()) return {};
  const int length =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

D2D1_COLOR_F ToColorF(COLORREF color, FLOAT alpha = 1.0f) {
  return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                      GetBValue(color) / 255.0f, alpha);
}

// The GPU was reset or removed; the whole device stack has to be rebuilt.
bool IsDeviceLost(HRESULT hr) {
  return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
         hr == D2DERR_RECREATE_TARGET;
}

}  // namespace

struct DebugWindow::RenderState {
  RenderingEngine engine;
  ComPtr<IDWriteTextFormat> format;
  UINT dpi{0};
  int line_height{1};
};

DebugWindow::DebugWindow() = default;

DebugWindow::~DebugWindow() { Destroy(); }

void DebugWindow::UpdateTheme() {
  theme_mode_ = CurrentThemeMode();
  theme_ = ResolveThemeColors(theme_mode_);
}

ATOM DebugWindow::RegisterWindowClass() {
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WndProc;
  window_class.hInstance = DebugWindowModule();
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kDebugWindowClass;
  const ATOM atom = RegisterClassExW(&window_class);
  return atom ? atom : (GetLastError() == ERROR_CLASS_ALREADY_EXISTS ? 1 : 0);
}

bool DebugWindow::Create() {
  if (hwnd_.load()) return GetCurrentThreadId() == ui_thread_id_;
  static const ATOM atom = RegisterWindowClass();
  if (!atom) return false;

  RECT work_area{0, 0, kDebugWindowWidth, kDebugWindowHeight};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
  const int x =
      std::max(static_cast<int>(work_area.left),
               static_cast<int>(work_area.right) - kDebugWindowWidth - kDebugWindowMargin);
  const int y = static_cast<int>(work_area.top) + kDebugWindowMargin;

  ui_thread_id_ = GetCurrentThreadId();
  // A device stack left by a Destroy from another thread belongs to the old window.
  render_.reset();
  // The surface's alpha makes the window semi-transparent (native-ui-spec §4.3).
  HWND hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
      kDebugWindowClass, L"azooKey debug", WS_POPUP, x, y, kDebugWindowWidth, kDebugWindowHeight,
      nullptr, nullptr, DebugWindowModule(), this);
  if (!hwnd) return false;
  UpdateTheme();
  hwnd_.store(hwnd);
  return true;
}

void DebugWindow::Destroy() {
  HWND hwnd = hwnd_.exchange(nullptr);
  if (!hwnd) return;
  // Detach first: from here on no message may reach this object, which the
  // caller is free to delete.
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
  if (GetCurrentThreadId() == ui_thread_id_) {
    DestroyWindow(hwnd);
    render_.reset();
  } else {
    // DestroyWindow fails off the owning thread; let that thread close it
    // (DefWindowProc turns WM_CLOSE into DestroyWindow).
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
  }
}

void DebugWindow::Show() {
  if (!hwnd_.load() && !Create()) return;
  ShowWindow(hwnd_.load(), SW_SHOWNOACTIVATE);
  Invalidate();
}

void DebugWindow::Hide() {
  if (HWND hwnd = hwnd_.load()) ShowWindow(hwnd, SW_HIDE);
}

void DebugWindow::Toggle() {
  if (IsVisible()) {
    Hide();
  } else {
    Show();
  }
}

bool DebugWindow::IsVisible() const {
  HWND hwnd = hwnd_.load();
  return hwnd && IsWindowVisible(hwnd);
}

void DebugWindow::Invalidate() {
  if (HWND hwnd = hwnd_.load()) InvalidateRect(hwnd, nullptr, TRUE);
}

void DebugWindow::SetPrivacyPolicy(const core::PrivacyPolicy& policy) {
  {
    std::lock_guard lock(policy_mutex_);
    paint_policy_ = policy;
  }
  Invalidate();
}

bool DebugWindow::BodyAllowedForPaint() const {
  std::lock_guard lock(policy_mutex_);
  return core::BodyLoggingAllowed(paint_policy_, core::BodyLogOptInFromEnvironment());
}

void DebugWindow::RecordIpc(DebugIpcLogEntry entry, const core::PrivacyPolicy& privacy) noexcept {
  try {
    const bool body_allowed =
        core::BodyLoggingAllowed(privacy, core::BodyLogOptInFromEnvironment());
    Mirror(buffer_.PushIpc(std::move(entry), body_allowed));
  } catch (...) {
    // Debug logging is best-effort and must never affect input handling.
  }
}

void DebugWindow::RecordTransition(DebugStateTransitionEntry entry) noexcept {
  try {
    Mirror(buffer_.PushTransition(std::move(entry)));
  } catch (...) {
    // Debug logging is best-effort and must never affect input handling.
  }
}

void DebugWindow::Mirror(const std::string& line) {
  // Keep the existing DebugView path alongside the window.
  OutputDebugStringW((L"[azooKey] " + DebugUtf8ToWide(line) + L"\n").c_str());
  Invalidate();
}

void DebugWindow::Paint(HWND hwnd) {
  // DirectComposition keeps the last frame; validate and draw the current lines.
  PAINTSTRUCT paint{};
  BeginPaint(hwnd, &paint);
  EndPaint(hwnd, &paint);
  try {
    Render(hwnd);
  } catch (...) {
    // Exceptions must not cross the window procedure; draw again next time.
    render_.reset();
  }
}

void DebugWindow::Render(HWND hwnd) {
  RECT client{};
  if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0) return;
  if (!render_) {
    auto next = std::make_unique<RenderState>();
    if (!next->engine.Initialize(hwnd, SurfaceAlpha::Premultiplied)) return;
    render_ = std::move(next);
  }
  RenderingEngine& engine = render_->engine;
  const UINT dpi = GetDpiForWindow(hwnd);
  if (!render_->format || render_->dpi != dpi) {
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteTextLayout> sample;
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(CreateMessageTextFormat(engine.write_factory(), dpi, &format)) ||
        FAILED(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)) ||
        FAILED(engine.write_factory()->CreateTextLayout(L"Ag", 2, format.Get(), 1000.0f, 1000.0f,
                                                        &sample)) ||
        FAILED(sample->GetMetrics(&metrics)))
      return;
    render_->format = std::move(format);
    render_->dpi = dpi;
    render_->line_height = std::max(1, static_cast<int>(std::ceil(metrics.height)));
  }
  if (!engine.ResizeSurface(client.right, client.bottom)) {
    if (IsDeviceLost(engine.failure_hr())) render_.reset();
    return;
  }
  ID2D1DeviceContext* context = engine.BeginDraw();
  if (!context) {
    if (IsDeviceLost(engine.failure_hr())) render_.reset();
    return;
  }
  // High contrast draws opaque (native-ui-spec §5.1).
  context->Clear(ToColorF(theme_.background, theme_mode_ == ThemeMode::HighContrast
                                                 ? 1.0f
                                                 : kDebugWindowAlpha / 255.0f));
  context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  ComPtr<ID2D1SolidColorBrush> brush;
  if (SUCCEEDED(context->CreateSolidColorBrush(ToColorF(theme_.text), &brush))) {
    const int line_height = render_->line_height;
    const int visible =
        std::max(1, (static_cast<int>(client.bottom) - kDebugWindowPadding * 2) / line_height);
    // Show the newest lines that fit; older ones remain in the ring buffer.
    const std::vector<std::string> lines = buffer_.RenderLines(BodyAllowedForPaint());
    const size_t first = lines.size() > static_cast<size_t>(visible)
                             ? lines.size() - static_cast<size_t>(visible)
                             : 0;
    int y = kDebugWindowPadding;
    for (size_t i = first; i < lines.size(); ++i) {
      const std::wstring text = DebugUtf8ToWide(lines[i]);
      context->DrawText(text.c_str(), static_cast<UINT32>(text.size()), render_->format.Get(),
                        D2D1::RectF(static_cast<FLOAT>(kDebugWindowPadding), static_cast<FLOAT>(y),
                                    static_cast<FLOAT>(client.right - kDebugWindowPadding),
                                    static_cast<FLOAT>(y + line_height)),
                        brush.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
      y += line_height;
    }
    brush->SetColor(ToColorF(theme_.border));
    context->DrawRectangle(D2D1::RectF(0.5f, 0.5f, static_cast<FLOAT>(client.right) - 0.5f,
                                       static_cast<FLOAT>(client.bottom) - 0.5f),
                           brush.Get());
  }
  if (!engine.EndDraw() && IsDeviceLost(engine.failure_hr())) render_.reset();
}

LRESULT CALLBACK DebugWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* self = reinterpret_cast<DebugWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (message) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      if (self) {
        self->Paint(hwnd);
        return 0;
      }
      break;
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      if (self && (message != WM_SETTINGCHANGE || IsThemeSettingChange(wparam, lparam))) {
        self->UpdateTheme();
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      break;
    case WM_NCDESTROY:
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      break;
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace azookey::tsf
