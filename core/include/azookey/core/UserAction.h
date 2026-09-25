#pragma once

#include <cstdint>

namespace azookey::core {

// Semantic key intent. Append-only (docs/legacy-parity-spec.md §1.5.2): new
// characters reuse Input / InputAlnum with a codepoint instead of new values.
enum class UserAction : uint16_t {
  // Character input.
  Input,
  InputAlnum,

  // Editing.
  Backspace,
  Delete,
  Forward,
  Backward,
  Up,
  Down,
  LineHead,
  LineEnd,

  // Conversion control.
  StartConversion,
  NextCandidate,
  PrevCandidate,
  SelectByDigit,
  Commit,
  Cancel,

  // Mode switching.
  ToggleHankaku,
  ToggleHiraKata,
  StartUnicodeInput,
  StartAlnumDouble,
  StartKanaDouble,

  // Learning.
  Forget,

  // Debugging.
  ToggleDebugWindow,
  ToggleAlnum,
};

inline constexpr uint32_t kModifierShift = 1u << 0;
inline constexpr uint32_t kModifierCtrl = 1u << 1;
inline constexpr uint32_t kModifierAlt = 1u << 2;
inline constexpr uint32_t kModifierWin = 1u << 3;

struct UserActionEvent {
  UserAction action{UserAction::Input};
  // Resolved by the TIP for Input / InputAlnum only.
  char32_t codepoint{0};
  uint32_t modifiers{0};
  // 1-9 for SelectByDigit.
  int digit{0};

  friend bool operator==(const UserActionEvent&, const UserActionEvent&) = default;
};

}  // namespace azookey::core
