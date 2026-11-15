#include "azookey/tsf/PromptDialog.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "azookey/tsf/DpiScaling.h"

namespace azookey::tsf {
namespace {

// The TIP DLL (or the test executable that links its objects).
HINSTANCE ModuleInstance() noexcept {
  HMODULE module = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&ModuleInstance), &module);
  return module;
}

constexpr wchar_t kClassName[] = L"AzooKeyPromptDialog";
constexpr wchar_t kPreviousProcProperty[] = L"AzooKeyPromptDialogPreviousProc";
constexpr int kIdPromptLabel = 1010;
constexpr int kIdSelectionLabel = 1011;
constexpr int kIdResultLabel = 1012;

// Layout in 96-DPI units; Layout() scales them to the window's DPI.
constexpr int kWidth = 440;
constexpr int kMargin = 12;
constexpr int kLabelHeight = 18;
constexpr int kEditHeight = 24;
constexpr int kTextHeight = 56;
constexpr int kStatusHeight = 36;
constexpr int kButtonWidth = 88;
constexpr int kButtonHeight = 26;
constexpr int kGap = 6;

ATOM RegisterPromptDialogClass(WNDPROC proc) noexcept {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = proc;
  wc.hInstance = ModuleInstance();
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = kClassName;
  return RegisterClassExW(&wc);
}

std::wstring WindowText(HWND hwnd) {
  if (!hwnd) return {};
  const int length = GetWindowTextLengthW(hwnd);
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(hwnd, text.data(), length + 1);
  text.resize(static_cast<size_t>(copied > 0 ? copied : 0));
  return text;
}

void SetTextIfChanged(HWND hwnd, const std::wstring& text) {
  if (hwnd && WindowText(hwnd) != text) SetWindowTextW(hwnd, text.c_str());
}

}  // namespace

PromptDialog::~PromptDialog() { Close(); }

bool PromptDialog::Create(HWND owner, Callbacks callbacks) noexcept {
  Close();
  static const ATOM atom = RegisterPromptDialogClass(&PromptDialog::WindowProc);
  if (!atom) return false;
  callbacks_ = std::move(callbacks);
  owner_ = owner;
  activated_ = false;
  closing_ = false;
  state_ = PromptDialogState::Editing;
  // The window and its controls are per-monitor aware (copilot-pc-backend-spec §7.2).
  const ScopedPerMonitorDpiAwareness dpi_context;
  HWND created =
      CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kClassName, L"AI変換",
                      WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT,
                      CW_USEDEFAULT, kWidth, 300, owner, nullptr, ModuleInstance(), this);
  if (!created) return false;
  hwnd_ = created;
  CreateControls();
  Layout();
  return true;
}

void PromptDialog::Show() noexcept {
  if (!hwnd_) return;
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
  if (HWND prompt = Control(kIdPrompt); prompt && IsWindowVisible(prompt)) {
    SetFocus(prompt);
    SendMessageW(prompt, EM_SETSEL, 0, -1);
  } else if (HWND cancel = Control(kIdCancel)) {
    SetFocus(cancel);
  }
}

void PromptDialog::Close() noexcept {
  if (closing_) return;
  if (hwnd_) {
    closing_ = true;
    DestroyWindow(std::exchange(hwnd_, nullptr));
    closing_ = false;
  }
  // Also after the window was destroyed with its owner.
  if (font_) DeleteObject(std::exchange(font_, nullptr));
}

HWND PromptDialog::Control(int id) const noexcept {
  return hwnd_ ? GetDlgItem(hwnd_, id) : nullptr;
}

void PromptDialog::CreateControls() {
  const auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND control = CreateWindowExW(cls == std::wstring_view(L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls,
                                   text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   ModuleInstance(), nullptr);
    if (control) {
      // Enter and Esc reach the window through its controls: no dialog
      // manager runs in the application's loop.
      SetPropW(control, kPreviousProcProperty,
               reinterpret_cast<HANDLE>(GetWindowLongPtrW(control, GWLP_WNDPROC)));
      SetWindowLongPtrW(control, GWLP_WNDPROC,
                        reinterpret_cast<LONG_PTR>(&PromptDialog::ControlProc));
    }
    return control;
  };
  child(L"STATIC", L"指示", 0, kIdPromptLabel);
  child(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdPrompt);
  child(L"STATIC", L"選択テキスト", 0, kIdSelectionLabel);
  child(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        kIdSelection);
  child(L"STATIC", L"結果", 0, kIdResultLabel);
  child(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        kIdResult);
  child(L"STATIC", L"", 0, kIdStatus);
  child(L"BUTTON", L"キャンセル", WS_TABSTOP | BS_PUSHBUTTON, kIdCancel);
  child(L"BUTTON", L"変換", WS_TABSTOP | BS_DEFPUSHBUTTON, kIdSubmit);
  child(L"BUTTON", L"適用", WS_TABSTOP | BS_PUSHBUTTON, kIdApply);
}

void PromptDialog::Layout(const RECT* suggested) {
  if (!hwnd_) return;
  // Positions are in the window's own (per-monitor) coordinates, whatever the
  // calling thread's awareness.
  const ScopedPerMonitorDpiAwareness dpi_context;
  const UINT dpi = NormalizeDpi(GetDpiForWindow(hwnd_));
  const auto s = [dpi](int value) { return ScaleForDpi(value, dpi); };
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
    if (font_) DeleteObject(font_);
    font_ = CreateFontIndirectW(&metrics.lfMessageFont);
  }
  const bool notice = state_ == PromptDialogState::Notice;
  const int inner = kWidth - 2 * kMargin;
  int y = kMargin;
  const auto place = [&](int id, int x, int top, int width, int height, bool visible) {
    if (HWND control = Control(id)) {
      SetWindowPos(control, nullptr, s(x), s(top), s(width), s(height), SWP_NOZORDER);
      ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
      if (font_) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    }
  };
  if (!notice) {
    place(kIdPromptLabel, kMargin, y, inner, kLabelHeight, true);
    y += kLabelHeight;
    place(kIdPrompt, kMargin, y, inner, kEditHeight, true);
    y += kEditHeight + kGap;
    place(kIdSelectionLabel, kMargin, y, inner, kLabelHeight, true);
    y += kLabelHeight;
    place(kIdSelection, kMargin, y, inner, kTextHeight, true);
    y += kTextHeight + kGap;
    place(kIdResultLabel, kMargin, y, inner, kLabelHeight, true);
    y += kLabelHeight;
    place(kIdResult, kMargin, y, inner, kTextHeight, true);
    y += kTextHeight + kGap;
  } else {
    for (const int id :
         {kIdPromptLabel, kIdPrompt, kIdSelectionLabel, kIdSelection, kIdResultLabel, kIdResult})
      place(id, 0, 0, 0, 0, false);
  }
  place(kIdStatus, kMargin, y, inner, kStatusHeight, true);
  y += kStatusHeight + kGap;
  const int right = kWidth - kMargin;
  place(kIdApply, right - kButtonWidth, y, kButtonWidth, kButtonHeight, !notice);
  place(kIdSubmit, right - 2 * kButtonWidth - kGap, y, kButtonWidth, kButtonHeight, !notice);
  place(kIdCancel, notice ? right - kButtonWidth : right - 3 * kButtonWidth - 2 * kGap, y,
        kButtonWidth, kButtonHeight, true);
  y += kButtonHeight + kMargin;

  RECT frame{0, 0, s(kWidth), s(y)};
  AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE,
                           static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)), dpi);
  const int width = frame.right - frame.left;
  const int height = frame.bottom - frame.top;
  if (suggested) {
    // A DPI change keeps the window where the system put it (copilot-pc-backend-spec §7.3).
    SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return;
  }
  // Centered on the owner, inside the work area of its monitor.
  RECT anchor{};
  if (!owner_ || !GetWindowRect(owner_, &anchor)) GetWindowRect(GetDesktopWindow(), &anchor);
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor);
  const RECT& work = monitor.rcWork;
  int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
  int top = anchor.top + ((anchor.bottom - anchor.top) - height) / 2;
  x = (std::max)(static_cast<int>(work.left), (std::min)(x, static_cast<int>(work.right) - width));
  top = (std::max)(static_cast<int>(work.top),
                   (std::min)(top, static_cast<int>(work.bottom) - height));
  SetWindowPos(hwnd_, nullptr, x, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void PromptDialog::Update(const PromptDialogView& view) noexcept {
  if (!hwnd_) return;
  try {
    const bool relayout =
        (state_ == PromptDialogState::Notice) != (view.state == PromptDialogState::Notice);
    state_ = view.state;
    HWND prompt = Control(kIdPrompt);
    const bool had_focus = GetFocus() != nullptr && IsChild(hwnd_, GetFocus());
    SetTextIfChanged(prompt, view.prompt);
    SetTextIfChanged(Control(kIdSelection), view.selection);
    SetTextIfChanged(Control(kIdResult), view.result);
    SetTextIfChanged(Control(kIdStatus), view.status);
    SetTextIfChanged(Control(kIdCancel),
                     view.state == PromptDialogState::Notice ? L"閉じる" : L"キャンセル");
    // A disabled control that holds the focus would leave Esc nowhere to go,
    // so the prompt turns read-only while waiting instead.
    if (prompt) SendMessageW(prompt, EM_SETREADONLY, view.prompt_editable ? FALSE : TRUE, 0);
    EnableWindow(Control(kIdSubmit), view.can_submit);
    EnableWindow(Control(kIdApply), view.can_apply);
    if (relayout) Layout();
    if (had_focus) {
      HWND focus = GetFocus();
      if (!focus || !IsWindowEnabled(focus) || !IsWindowVisible(focus))
        SetFocus(prompt && IsWindowVisible(prompt) ? prompt : Control(kIdCancel));
    }
  } catch (...) {
  }
}

void PromptDialog::Submit() {
  if (!callbacks_.submit || !IsWindowEnabled(Control(kIdSubmit))) return;
  callbacks_.submit(WindowText(Control(kIdPrompt)));
}

void PromptDialog::Apply() {
  if (!callbacks_.apply || !IsWindowEnabled(Control(kIdApply))) return;
  callbacks_.apply();
}

void PromptDialog::Cancel() {
  if (closing_ || !callbacks_.cancel) return;
  callbacks_.cancel();
}

LRESULT CALLBACK PromptDialog::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  auto* self = reinterpret_cast<PromptDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self || (self->hwnd_ != hwnd && message != WM_DESTROY))
    return DefWindowProcW(hwnd, message, wparam, lparam);
  try {
    return self->HandleMessage(hwnd, message, wparam, lparam);
  } catch (...) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

LRESULT PromptDialog::HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_COMMAND:
      if (HIWORD(wparam) == BN_CLICKED) {
        switch (LOWORD(wparam)) {
          case kIdSubmit:
            Submit();
            return 0;
          case kIdApply:
            Apply();
            return 0;
          case kIdCancel:
            Cancel();
            return 0;
          default:
            break;
        }
      }
      break;
    case WM_ACTIVATE:
      if (LOWORD(wparam) == WA_INACTIVE) {
        // Losing activation ends the window and its request (legacy-parity-spec §4.3).
        if (activated_ && !closing_) Cancel();
        return 0;
      }
      activated_ = true;
      break;
    case WM_CLOSE:
      Cancel();
      return 0;
    case WM_DPICHANGED:
      Layout(reinterpret_cast<const RECT*>(lparam));
      return 0;
    case WM_DESTROY:
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      if (hwnd_ == hwnd) {
        // Destroyed from outside, with its owner: the request ends with it.
        hwnd_ = nullptr;
        Cancel();
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK PromptDialog::ControlProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  const auto previous = reinterpret_cast<WNDPROC>(GetPropW(hwnd, kPreviousProcProperty));
  HWND parent = GetParent(hwnd);
  auto* self =
      parent ? reinterpret_cast<PromptDialog*>(GetWindowLongPtrW(parent, GWLP_USERDATA)) : nullptr;
  if (message == WM_NCDESTROY) {
    RemovePropW(hwnd, kPreviousProcProperty);
    if (previous) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
  } else if (self && (message == WM_KEYDOWN || message == WM_CHAR)) {
    const bool enter = message == WM_KEYDOWN ? wparam == VK_RETURN : wparam == L'\r';
    const bool escape = message == WM_KEYDOWN ? wparam == VK_ESCAPE : wparam == 0x1B;
    if (escape) {
      if (message == WM_KEYDOWN) self->Cancel();
      return 0;
    }
    if (enter && GetDlgCtrlID(hwnd) == kIdPrompt) {
      // Enter applies a shown result, or sends the prompt.
      if (message == WM_KEYDOWN) {
        if (self->state_ == PromptDialogState::Preview && IsWindowEnabled(self->Control(kIdApply)))
          self->Apply();
        else
          self->Submit();
      }
      return 0;
    }
  }
  return previous ? CallWindowProcW(previous, hwnd, message, wparam, lparam)
                  : DefWindowProcW(hwnd, message, wparam, lparam);
}

#ifdef AZOOKEY_TSF_TESTING
std::wstring PromptDialog::text_for_test(int id) const { return WindowText(Control(id)); }
bool PromptDialog::enabled_for_test(int id) const {
  HWND control = Control(id);
  return control && IsWindowEnabled(control);
}
bool PromptDialog::visible_for_test(int id) const {
  HWND control = Control(id);
  return control && (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
}
bool PromptDialog::read_only_for_test(int id) const {
  HWND control = Control(id);
  return control && (GetWindowLongPtrW(control, GWL_STYLE) & ES_READONLY) != 0;
}
void PromptDialog::set_text_for_test(int id, const std::wstring& text) {
  if (HWND control = Control(id)) SetWindowTextW(control, text.c_str());
}
void PromptDialog::command_for_test(int id) {
  if (hwnd_)
    SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
                 reinterpret_cast<LPARAM>(Control(id)));
}
void PromptDialog::key_for_test(int id, WPARAM key) {
  if (HWND control = Control(id)) SendMessageW(control, WM_KEYDOWN, key, 0);
}
void PromptDialog::deactivate_for_test() {
  if (!hwnd_) return;
  activated_ = true;
  SendMessageW(hwnd_, WM_ACTIVATE, WA_INACTIVE, 0);
}
#endif

}  // namespace azookey::tsf
