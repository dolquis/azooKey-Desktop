#include "azookey/tsf/DebugWindow.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "azookey/core/BodyLogGate.h"

namespace azookey::tsf {
namespace {

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

}  // namespace

DebugWindow::~DebugWindow() { Destroy(); }

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
  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                              kDebugWindowClass, L"azooKey debug", WS_POPUP | WS_BORDER, x, y,
                              kDebugWindowWidth, kDebugWindowHeight, nullptr, nullptr,
                              DebugWindowModule(), this);
  if (!hwnd) return false;
  SetLayeredWindowAttributes(hwnd, 0, kDebugWindowAlpha, LWA_ALPHA);
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
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(hwnd, &paint);
  if (!dc) return;
  RECT client{};
  GetClientRect(hwnd, &client);
  FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(0xE0, 0xE0, 0xE0));
  HGDIOBJ previous_font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));

  TEXTMETRICW metrics{};
  GetTextMetricsW(dc, &metrics);
  const int line_height = std::max(1, static_cast<int>(metrics.tmHeight));
  const int visible =
      std::max(1, (static_cast<int>(client.bottom) - kDebugWindowPadding * 2) / line_height);

  try {
    // Show the newest lines that fit; older ones remain in the ring buffer.
    const std::vector<std::string> lines = buffer_.RenderLines(BodyAllowedForPaint());
    const size_t first = lines.size() > static_cast<size_t>(visible)
                             ? lines.size() - static_cast<size_t>(visible)
                             : 0;
    int y = kDebugWindowPadding;
    for (size_t i = first; i < lines.size(); ++i) {
      const std::wstring text = DebugUtf8ToWide(lines[i]);
      TextOutW(dc, kDebugWindowPadding, y, text.c_str(), static_cast<int>(text.size()));
      y += line_height;
    }
  } catch (...) {
    // Exceptions must not cross the window procedure; leave the frame blank.
  }

  SelectObject(dc, previous_font);
  EndPaint(hwnd, &paint);
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
    case WM_NCDESTROY:
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      break;
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace azookey::tsf
