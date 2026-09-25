#include "azookey/core/UserActionMap.h"

namespace azookey::core {
namespace {

bool IsComposing(InputStateKind kind) {
  return kind == InputStateKind::Composing || kind == InputStateKind::Previewing;
}

// States that own a reading or hex buffer edited by character keys.
bool AcceptsEditing(InputStateKind kind) {
  return IsComposing(kind) || kind == InputStateKind::Selecting ||
         kind == InputStateKind::UnicodeInput;
}

bool IsLetter(uint32_t key) { return key >= vk::kA && key <= vk::kZ; }
bool IsDigitRow(uint32_t key) { return key >= vk::k0 && key <= vk::k9; }
bool IsNumpadDigit(uint32_t key) { return key >= vk::kNumpad0 && key <= vk::kNumpad9; }
bool IsCompositionSymbol(uint32_t key) {
  return key == vk::kOemMinus || key == vk::kSubtract || key == vk::kOemComma ||
         key == vk::kOemPeriod || key == vk::kOem2;
}

std::optional<UserActionEvent> Make(UserAction action, uint32_t modifiers, int digit = 0) {
  return UserActionEvent{action, 0, modifiers, digit};
}

std::optional<UserActionEvent> MapCtrl(uint32_t key, uint32_t modifiers, InputStateKind kind) {
  if ((modifiers & kModifierShift) != 0) {
    if (key == 'U' && kind != InputStateKind::ReplaceSuggestion &&
        kind != InputStateKind::UnicodeInput)
      return Make(UserAction::StartUnicodeInput, modifiers);
    if (key == vk::kBack && kind == InputStateKind::Idle)
      return Make(UserAction::Forget, modifiers);
    return std::nullopt;
  }
  switch (key) {
    case 'H':
      if (AcceptsEditing(kind)) return Make(UserAction::Backspace, modifiers);
      break;
    case 'P':
      if (kind == InputStateKind::Selecting) return Make(UserAction::Up, modifiers);
      break;
    case 'N':
      if (kind == InputStateKind::Selecting) return Make(UserAction::Down, modifiers);
      break;
    case 'F':
      if (kind == InputStateKind::Selecting) return Make(UserAction::Forward, modifiers);
      break;
    case 'B':
      if (kind == InputStateKind::Selecting) return Make(UserAction::Backward, modifiers);
      break;
    default:
      // Ctrl+A / Ctrl+E (LineHead / LineEnd) wait for a cursor inside the
      // composition; Ctrl+I / O / S are reserved for script conversions.
      break;
  }
  return std::nullopt;
}

}  // namespace

std::optional<UserActionEvent> MapUserAction(uint32_t virtual_key, uint32_t modifiers,
                                             InputStateKind kind) {
  if ((modifiers & (kModifierAlt | kModifierWin)) != 0) return std::nullopt;
  if ((modifiers & kModifierCtrl) != 0) return MapCtrl(virtual_key, modifiers, kind);

  const bool shift = (modifiers & kModifierShift) != 0;
  if (!shift) {
    switch (virtual_key) {
      case vk::kKanji:
      case vk::kOemAuto:
        return Make(UserAction::ToggleHankaku, modifiers);
      case vk::kNonConvert:
        return Make(UserAction::ToggleHiraKata, modifiers);
      case vk::kOemAttn:
        return Make(UserAction::ToggleAlnum, modifiers);
      case vk::kF10:
        return Make(UserAction::ToggleDebugWindow, modifiers);
      default:
        break;
    }
  }

  if (kind == InputStateKind::UnicodeInput &&
      (IsLetter(virtual_key) || IsDigitRow(virtual_key) || IsNumpadDigit(virtual_key) ||
       IsCompositionSymbol(virtual_key)))
    return Make(UserAction::Input, modifiers);

  if (IsLetter(virtual_key)) {
    if (kind == InputStateKind::ReplaceSuggestion) return std::nullopt;
    return Make(UserAction::Input, modifiers);
  }
  if (IsDigitRow(virtual_key)) {
    // Digits select only while the window is visible; elsewhere they pass
    // through unless the TIP's number rewriter claims them.
    if (kind == InputStateKind::Selecting && virtual_key != vk::k0)
      return Make(UserAction::SelectByDigit, modifiers, static_cast<int>(virtual_key - vk::k0));
    return std::nullopt;
  }
  if (IsCompositionSymbol(virtual_key)) {
    if (IsComposing(kind) || kind == InputStateKind::Selecting ||
        (kind == InputStateKind::Idle && !shift &&
         (virtual_key == vk::kOemComma || virtual_key == vk::kOemPeriod)))
      return Make(UserAction::Input, modifiers);
    return std::nullopt;
  }

  switch (virtual_key) {
    case vk::kBack:
      if (AcceptsEditing(kind)) return Make(UserAction::Backspace, modifiers);
      break;
    case vk::kSpace:
      if (IsComposing(kind)) return Make(UserAction::StartConversion, modifiers);
      if (kind == InputStateKind::Selecting)
        return Make(shift ? UserAction::PrevCandidate : UserAction::NextCandidate, modifiers);
      break;
    case vk::kConvert:
      if (!shift && IsComposing(kind)) return Make(UserAction::StartConversion, modifiers);
      if (!shift && kind == InputStateKind::Selecting)
        return Make(UserAction::NextCandidate, modifiers);
      break;
    case vk::kUp:
      if (kind == InputStateKind::Selecting) return Make(UserAction::Up, modifiers);
      break;
    case vk::kDown:
      if (kind == InputStateKind::Selecting) return Make(UserAction::Down, modifiers);
      break;
    case vk::kLeft:
      if (kind == InputStateKind::Selecting) return Make(UserAction::Backward, modifiers);
      break;
    case vk::kRight:
      if (kind == InputStateKind::Selecting) return Make(UserAction::Forward, modifiers);
      break;
    case vk::kReturn:
      if (kind != InputStateKind::Idle) return Make(UserAction::Commit, modifiers);
      break;
    case vk::kEscape:
      if (kind != InputStateKind::Idle) return Make(UserAction::Cancel, modifiers);
      break;
    default:
      // Home / End / Delete wait for a cursor inside the composition.
      break;
  }
  return std::nullopt;
}

}  // namespace azookey::core
