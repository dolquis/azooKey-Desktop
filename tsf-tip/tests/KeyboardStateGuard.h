#pragma once

#include <Windows.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <mutex>

#include "azookey/tsf/TextService.h"

namespace azookey::tsf::test {

// TextService reads Shift / Ctrl / Alt / Win through GetKeyState on the calling
// thread, so a modifier left in that thread's key state turns an ordinary key
// into an unhandled chord that is not eaten (DEV-1393). The guard pins every
// modifier to "up" on construction, remembers the state a test asks for with
// SetDown, and Reapply() writes that state back right before each key press.
//
// The OS can also sync the thread's key state with real input on the desktop
// (a modifier someone holds) between Reapply() and the key, or while the key
// runs (DEV-1828). So the guard also serves TextService's key-state reads and
// writes through KeyStateHooksForTest: the modifiers it manages come from the
// guard alone, and every other key from the thread as before.
// The hooks are process-wide, but active guards belong to a single thread.
class KeyboardStateGuard {
 public:
  KeyboardStateGuard() {
    const std::lock_guard lock(current_mutex_);
    if (current_ && !current_->CheckOwnerThread()) std::abort();
    has_original_ = GetKeyboardState(original_.data()) != FALSE;
    EXPECT_TRUE(has_original_) << "GetKeyboardState failed: " << GetLastError();
    for (const int vk : kModifierKeys) managed_[static_cast<size_t>(vk)] = true;
    Reapply();
    previous_ = current_;
    current_ = this;
    azookey::tsf::testing::SetKeyStateHooksForTest(&kHooks);
  }

  // Guards nest last-in, first-out only.
  ~KeyboardStateGuard() {
    if (!CheckOwnerThread()) std::abort();
    const std::lock_guard lock(current_mutex_);
    EXPECT_EQ(current_, this) << "KeyboardStateGuard destroyed out of order";
    current_ = previous_;
    azookey::tsf::testing::SetKeyStateHooksForTest(current_ ? &kHooks : nullptr);
    if (has_original_) SetKeyboardState(original_.data());
  }

  KeyboardStateGuard(const KeyboardStateGuard&) = delete;
  KeyboardStateGuard& operator=(const KeyboardStateGuard&) = delete;

  void SetDown(int vk, bool down) {
    if (!CheckOwnerThread()) return;
    const auto index = static_cast<size_t>(vk);
    managed_[index] = true;
    pinned_down_[index] = down;
    Reapply();
  }

  // The key state TextService reads, for a test's own translation hook: the
  // thread's state with the current guard's modifiers over it.
  static BOOL ReadKeyboardState(PBYTE state) { return HookGetKeyboardState(state); }

  void ClearSystemModifiers() {
    if (!CheckOwnerThread()) return;
    SetDown(VK_CONTROL, false);
    SetDown(VK_LCONTROL, false);
    SetDown(VK_RCONTROL, false);
    SetDown(VK_MENU, false);
    SetDown(VK_LMENU, false);
    SetDown(VK_RMENU, false);
    SetDown(VK_LWIN, false);
    SetDown(VK_RWIN, false);
  }

  // Writes the pinned down/up bits of the managed keys back into the thread's
  // key state and into the state TextService reads, keeping the toggle bits
  // and every other key as they are.
  void Reapply() {
    if (!CheckOwnerThread()) return;
    down_ = pinned_down_;
    std::array<BYTE, 256> state{};
    if (!GetKeyboardState(state.data())) {
      ADD_FAILURE() << "GetKeyboardState failed: " << GetLastError();
      return;
    }
    Overlay(state.data());
    if (!SetKeyboardState(state.data()))
      ADD_FAILURE() << "SetKeyboardState failed: " << GetLastError();
  }

 private:
  static constexpr std::array<int, 11> kModifierKeys{
      VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
      VK_MENU,  VK_LMENU,  VK_RMENU,  VK_LWIN,    VK_RWIN};

  bool CheckOwnerThread() const {
    const DWORD caller = GetCurrentThreadId();
    if (caller == owner_thread_id_) return true;
    ADD_FAILURE() << "KeyboardStateGuard belongs to thread " << owner_thread_id_
                  << " but was accessed on thread " << caller;
    return false;
  }

  void Overlay(BYTE* state) const {
    for (size_t index = 0; index < down_.size(); ++index) {
      if (!managed_[index]) continue;
      state[index] = down_[index] ? static_cast<BYTE>(state[index] | 0x80)
                                  : static_cast<BYTE>(state[index] & 0x7f);
    }
  }

  static SHORT HookGetKeyState(int vk) {
    const std::lock_guard lock(current_mutex_);
    if (current_ && !current_->CheckOwnerThread()) return 0;
    const SHORT real = GetKeyState(vk);
    const auto index = static_cast<size_t>(vk) & 0xff;
    if (!current_ || !current_->managed_[index]) return real;
    const USHORT toggle = static_cast<USHORT>(real) & 0x0001;
    return static_cast<SHORT>(toggle | (current_->down_[index] ? 0x8000 : 0));
  }

  static BOOL HookGetKeyboardState(PBYTE state) {
    const std::lock_guard lock(current_mutex_);
    if (current_ && !current_->CheckOwnerThread()) return FALSE;
    if (!GetKeyboardState(state)) return FALSE;
    if (current_) current_->Overlay(state);
    return TRUE;
  }

  // A replayed key runs with the key state saved for it, then the current one
  // is put back; the managed modifiers follow both.
  static BOOL HookSetKeyboardState(LPBYTE state) {
    const std::lock_guard lock(current_mutex_);
    if (current_) {
      if (!current_->CheckOwnerThread()) return FALSE;
      for (size_t index = 0; index < current_->down_.size(); ++index)
        if (current_->managed_[index]) current_->down_[index] = (state[index] & 0x80) != 0;
    }
    return SetKeyboardState(state);
  }

  static constexpr azookey::tsf::testing::KeyStateHooksForTest kHooks{
      &HookGetKeyState, &HookGetKeyboardState, &HookSetKeyboardState};
  // A hook can already be in flight when the guard unregisters it.
  static inline std::mutex current_mutex_;
  static inline KeyboardStateGuard* current_ = nullptr;

  const DWORD owner_thread_id_ = GetCurrentThreadId();
  std::array<BYTE, 256> original_{};
  bool has_original_{false};
  std::array<bool, 256> managed_{};
  std::array<bool, 256> pinned_down_{};
  // The managed modifiers as TextService sees them: the pinned state after
  // Reapply(), or what TextService last wrote with SetKeyboardState.
  std::array<bool, 256> down_{};
  KeyboardStateGuard* previous_{nullptr};
};

}  // namespace azookey::tsf::test
