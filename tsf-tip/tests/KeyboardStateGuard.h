#pragma once

#include <Windows.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace azookey::tsf::test {

// TextService reads Shift / Ctrl / Alt / Win through GetKeyState on the calling
// thread, so a modifier left in that thread's key state turns an ordinary key
// into an unhandled chord that is not eaten (DEV-1393). The guard pins every
// modifier to "up" on construction, remembers the state a test asks for with
// SetDown, and Reapply() writes that state back right before each key press.
class KeyboardStateGuard {
 public:
  KeyboardStateGuard() {
    has_original_ = GetKeyboardState(original_.data()) != FALSE;
    EXPECT_TRUE(has_original_) << "GetKeyboardState failed: " << GetLastError();
    for (const int vk : kModifierKeys) managed_[static_cast<size_t>(vk)] = true;
    Reapply();
  }

  ~KeyboardStateGuard() {
    if (has_original_) SetKeyboardState(original_.data());
  }

  KeyboardStateGuard(const KeyboardStateGuard&) = delete;
  KeyboardStateGuard& operator=(const KeyboardStateGuard&) = delete;

  void SetDown(int vk, bool down) {
    const auto index = static_cast<size_t>(vk);
    managed_[index] = true;
    pinned_down_[index] = down;
    Reapply();
  }

  void ClearSystemModifiers() {
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
  // key state, keeping the toggle bits and every other key as they are.
  void Reapply() {
    std::array<BYTE, 256> state{};
    if (!GetKeyboardState(state.data())) {
      ADD_FAILURE() << "GetKeyboardState failed: " << GetLastError();
      return;
    }
    for (size_t index = 0; index < state.size(); ++index) {
      if (!managed_[index]) continue;
      state[index] = pinned_down_[index] ? static_cast<BYTE>(state[index] | 0x80)
                                         : static_cast<BYTE>(state[index] & 0x7f);
    }
    if (!SetKeyboardState(state.data()))
      ADD_FAILURE() << "SetKeyboardState failed: " << GetLastError();
  }

 private:
  static constexpr std::array<int, 11> kModifierKeys{
      VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
      VK_MENU,  VK_LMENU,  VK_RMENU,  VK_LWIN,    VK_RWIN};

  std::array<BYTE, 256> original_{};
  bool has_original_{false};
  std::array<bool, 256> managed_{};
  std::array<bool, 256> pinned_down_{};
};

}  // namespace azookey::tsf::test
