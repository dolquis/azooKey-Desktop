#pragma once

#include <cstdint>
#include <optional>

#include "azookey/core/InputState.h"
#include "azookey/core/UserAction.h"

namespace azookey::core {

// Windows virtual-key codes used by the map. The core builds without
// <windows.h>; tsf-tip/tests/keymap_test.cpp checks these against VK_*.
namespace vk {
inline constexpr uint32_t kBack = 0x08;
inline constexpr uint32_t kReturn = 0x0D;
inline constexpr uint32_t kKanji = 0x19;
inline constexpr uint32_t kEscape = 0x1B;
inline constexpr uint32_t kConvert = 0x1C;
inline constexpr uint32_t kNonConvert = 0x1D;
inline constexpr uint32_t kSpace = 0x20;
inline constexpr uint32_t kEnd = 0x23;
inline constexpr uint32_t kHome = 0x24;
inline constexpr uint32_t kLeft = 0x25;
inline constexpr uint32_t kUp = 0x26;
inline constexpr uint32_t kRight = 0x27;
inline constexpr uint32_t kDown = 0x28;
inline constexpr uint32_t kDelete = 0x2E;
inline constexpr uint32_t k0 = 0x30;
inline constexpr uint32_t k9 = 0x39;
inline constexpr uint32_t kA = 0x41;
inline constexpr uint32_t kZ = 0x5A;
inline constexpr uint32_t kNumpad0 = 0x60;
inline constexpr uint32_t kNumpad9 = 0x69;
inline constexpr uint32_t kSubtract = 0x6D;
inline constexpr uint32_t kF10 = 0x79;
inline constexpr uint32_t kOemComma = 0xBC;
inline constexpr uint32_t kOemMinus = 0xBD;
inline constexpr uint32_t kOemPeriod = 0xBE;
inline constexpr uint32_t kOem2 = 0xBF;
inline constexpr uint32_t kOemAttn = 0xF0;
inline constexpr uint32_t kOemAuto = 0xF3;
inline constexpr uint32_t kOemEnlw = 0xF4;
}  // namespace vk

// First layer of the key translation (docs/legacy-parity-spec.md §1.5.3):
// (VK, modifiers, current kind) -> the semantic action, or nullopt when the
// IME does not consume the key in that state. Pure and layout independent;
// the TIP resolves Input codepoints and applies setting- or hint-dependent
// overrides (number rewriter digits, batch segments, bracket pairing).
std::optional<UserActionEvent> MapUserAction(uint32_t virtual_key, uint32_t modifiers,
                                             InputStateKind kind);

}  // namespace azookey::core
