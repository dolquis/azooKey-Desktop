#pragma once

#include <cstdint>

#include "azookey/core/UserAction.h"
#include "azookey/core/UserActionMap.h"

namespace azookey::tsf {

// settings.json convertKeysAction (docs/tsf-deep-integration-spec.md §3.1.1).
enum class ConvertKeysAction : uint8_t {
  // Convert reconverts and NonConvert cycles the character form (§3.2, §3.3).
  Default,
  // Convert turns the IME on and NonConvert turns it off.
  ImeOnOff,
};

// What a key does to the keyboard open/close state before any other handling.
enum class OpenCloseKeyAction : uint8_t {
  None,
  // Hankaku/Zenkaku: flips the state.
  Toggle,
  // VK_IME_ON, the hiragana key (VK_DBE_HIRAGANA), or Convert under ImeOnOff:
  // on, and hiragana when alphanumeric.
  Open,
  // VK_IME_OFF, or NonConvert under ImeOnOff: off, committing what is shown.
  Close,
};

// Keys with any modifier are not open/close keys, as Hankaku/Zenkaku is not.
constexpr OpenCloseKeyAction ClassifyOpenCloseKey(uint32_t virtual_key, uint32_t modifiers,
                                                  ConvertKeysAction convert_keys) {
  if ((modifiers &
       (core::kModifierCtrl | core::kModifierAlt | core::kModifierWin | core::kModifierShift)) != 0)
    return OpenCloseKeyAction::None;
  switch (virtual_key) {
    case core::vk::kKanji:
    case core::vk::kOemAuto:
    case core::vk::kOemEnlw:
      return OpenCloseKeyAction::Toggle;
    case core::vk::kImeOn:
    case core::vk::kDbeHiragana:
      return OpenCloseKeyAction::Open;
    case core::vk::kImeOff:
      return OpenCloseKeyAction::Close;
    case core::vk::kConvert:
      return convert_keys == ConvertKeysAction::ImeOnOff ? OpenCloseKeyAction::Open
                                                         : OpenCloseKeyAction::None;
    case core::vk::kNonConvert:
      return convert_keys == ConvertKeysAction::ImeOnOff ? OpenCloseKeyAction::Close
                                                         : OpenCloseKeyAction::None;
    default:
      return OpenCloseKeyAction::None;
  }
}

}  // namespace azookey::tsf
