#include "azookey/tsf/DelayedActionTimer.h"

namespace azookey::tsf {

namespace {
constexpr wchar_t kWindowClass[] = L"AzooKeyTipDelayedActionTimer";
// SetTimer ids are 1-based per window; Slot is 0-based.
constexpr UINT_PTR TimerId(DelayedActionTimer::Slot slot) {
  return static_cast<UINT_PTR>(slot) + 1;
}

bool EnsureWindowClass(HINSTANCE instance, WNDPROC proc) {
  WNDCLASSEXW existing{};
  if (GetClassInfoExW(instance, kWindowClass, &existing)) return true;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = proc;
  wc.hInstance = instance;
  wc.lpszClassName = kWindowClass;
  return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}
}  // namespace

DelayedActionTimer::~DelayedActionTimer() { Destroy(); }

bool DelayedActionTimer::Create(Callback callback, void* context) {
  if (hwnd_) return true;
  HINSTANCE instance = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&DelayedActionTimer::WindowProc), &instance);
  if (!EnsureWindowClass(instance, &DelayedActionTimer::WindowProc)) return false;
  callback_ = callback;
  context_ = context;
  hwnd_ =
      CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
  return hwnd_ != nullptr;
}

void DelayedActionTimer::Destroy() {
  if (!hwnd_) return;
  CancelAll();
  DestroyWindow(hwnd_);
  hwnd_ = nullptr;
  callback_ = nullptr;
  context_ = nullptr;
}

bool DelayedActionTimer::Arm(Slot slot, uint32_t delay_ms) {
  if (!hwnd_ || slot >= kSlotCount || delay_ms == 0) return false;
  if (SetTimer(hwnd_, TimerId(slot), delay_ms, nullptr) == 0) {
    armed_[slot] = false;
    return false;
  }
  armed_[slot] = true;
  delay_ms_[slot] = delay_ms;
  return true;
}

void DelayedActionTimer::Cancel(Slot slot) {
  if (slot >= kSlotCount) return;
  if (hwnd_) KillTimer(hwnd_, TimerId(slot));
  armed_[slot] = false;
}

void DelayedActionTimer::CancelAll() {
  Cancel(kIdle);
  Cancel(kFinal);
}

void DelayedActionTimer::FireForTest(Slot slot) {
  if (slot < kSlotCount && armed_[slot]) OnTimer(slot);
}

void DelayedActionTimer::OnTimer(Slot slot) {
  // KillTimer does not remove a WM_TIMER already queued, so an expiry that was
  // cancelled or re-armed after it was posted must not fire.
  if (!armed_[slot]) return;
  // One-shot: disarm before the callback so it may re-arm the same slot.
  Cancel(slot);
  if (callback_) callback_(context_, slot);
}

LRESULT CALLBACK DelayedActionTimer::WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  } else if (msg == WM_TIMER) {
    auto* self = reinterpret_cast<DelayedActionTimer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && wparam >= 1 && wparam <= kSlotCount) {
      self->OnTimer(static_cast<Slot>(wparam - 1));
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace azookey::tsf
