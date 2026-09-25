#include <gtest/gtest.h>
#include <windows.h>

#include <iterator>
#include <optional>
#include <vector>

#include "azookey/core/UserActionMap.h"

namespace {

using azookey::core::InputStateKind;
using azookey::core::MapUserAction;
using azookey::core::UserAction;
using azookey::core::UserActionEvent;
namespace vk = azookey::core::vk;

constexpr uint32_t kShift = azookey::core::kModifierShift;
constexpr uint32_t kCtrl = azookey::core::kModifierCtrl;
constexpr uint32_t kAlt = azookey::core::kModifierAlt;
constexpr uint32_t kWin = azookey::core::kModifierWin;

using K = InputStateKind;
constexpr K kAllKinds[] = {K::Idle,      K::Composing,         K::Previewing,
                           K::Selecting, K::ReplaceSuggestion, K::UnicodeInput};

std::optional<UserAction> Action(uint32_t key, uint32_t modifiers, K kind) {
  const auto event = MapUserAction(key, modifiers, kind);
  if (!event) return std::nullopt;
  return event->action;
}

struct Entry {
  uint32_t key;
  uint32_t modifiers;
  // Expected action per kind, in kAllKinds order. nullopt = pass through.
  std::optional<UserAction> expected[std::size(kAllKinds)];
};

constexpr auto kNone = std::nullopt;
using A = UserAction;

// docs/legacy-parity-spec.md §1.4 table, resolved per state (§1.5.3).
const Entry kEntries[] = {
    //                          Idle  Composing  Previewing  Selecting  Replace  Unicode
    {'A', 0, {A::Input, A::Input, A::Input, A::Input, kNone, A::Input}},
    {'Z', kShift, {A::Input, A::Input, A::Input, A::Input, kNone, A::Input}},
    {VK_BACK, 0, {kNone, A::Backspace, A::Backspace, A::Backspace, kNone, A::Backspace}},
    {VK_DELETE, 0, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {VK_LEFT, 0, {kNone, kNone, kNone, A::Backward, kNone, kNone}},
    {VK_RIGHT, 0, {kNone, kNone, kNone, A::Forward, kNone, kNone}},
    {VK_UP, 0, {kNone, kNone, kNone, A::Up, kNone, kNone}},
    {VK_DOWN, 0, {kNone, kNone, kNone, A::Down, kNone, kNone}},
    {VK_HOME, 0, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {VK_END, 0, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {VK_SPACE, 0, {kNone, A::StartConversion, A::StartConversion, A::NextCandidate, kNone, kNone}},
    {VK_SPACE,
     kShift,
     {kNone, A::StartConversion, A::StartConversion, A::PrevCandidate, kNone, kNone}},
    {VK_RETURN, 0, {kNone, A::Commit, A::Commit, A::Commit, A::Commit, A::Commit}},
    {VK_ESCAPE, 0, {kNone, A::Cancel, A::Cancel, A::Cancel, A::Cancel, A::Cancel}},
    {'1', 0, {kNone, kNone, kNone, A::SelectByDigit, kNone, A::Input}},
    {'9', kShift, {kNone, kNone, kNone, A::SelectByDigit, kNone, A::Input}},
    {'0', 0, {kNone, kNone, kNone, kNone, kNone, A::Input}},
    {VK_NUMPAD5, 0, {kNone, kNone, kNone, kNone, kNone, A::Input}},
    {VK_OEM_MINUS, 0, {kNone, A::Input, A::Input, A::Input, kNone, A::Input}},
    {VK_SUBTRACT, 0, {kNone, A::Input, A::Input, A::Input, kNone, A::Input}},
    {VK_OEM_COMMA, 0, {A::Input, A::Input, A::Input, A::Input, kNone, A::Input}},
    {VK_OEM_PERIOD, 0, {A::Input, A::Input, A::Input, A::Input, kNone, A::Input}},
    {VK_OEM_2, kShift, {kNone, A::Input, A::Input, A::Input, kNone, A::Input}},
    {'H', kCtrl, {kNone, A::Backspace, A::Backspace, A::Backspace, kNone, A::Backspace}},
    {'P', kCtrl, {kNone, kNone, kNone, A::Up, kNone, kNone}},
    {'N', kCtrl, {kNone, kNone, kNone, A::Down, kNone, kNone}},
    {'F', kCtrl, {kNone, kNone, kNone, A::Forward, kNone, kNone}},
    {'B', kCtrl, {kNone, kNone, kNone, A::Backward, kNone, kNone}},
    {'A', kCtrl, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {'E', kCtrl, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {'I', kCtrl, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {'O', kCtrl, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {'S', kCtrl, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {'U',
     kCtrl | kShift,
     {A::StartUnicodeInput, A::StartUnicodeInput, A::StartUnicodeInput, A::StartUnicodeInput, kNone,
      kNone}},
    {'U', kCtrl, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {VK_BACK, kCtrl | kShift, {A::Forget, kNone, kNone, kNone, kNone, kNone}},
    {VK_KANJI,
     0,
     {A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku,
      A::ToggleHankaku}},
    {VK_OEM_AUTO,
     0,
     {A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku,
      A::ToggleHankaku}},
    {VK_NONCONVERT,
     0,
     {A::ToggleHiraKata, A::ToggleHiraKata, A::ToggleHiraKata, A::ToggleHiraKata, A::ToggleHiraKata,
      A::ToggleHiraKata}},
    {VK_CONVERT, 0, {kNone, A::StartConversion, A::StartConversion, A::NextCandidate, kNone, kNone}},
    {VK_OEM_ATTN, 0, {A::ToggleAlnum, A::ToggleAlnum, A::ToggleAlnum, A::ToggleAlnum,
                       A::ToggleAlnum, A::ToggleAlnum}},
    {VK_F10,
     0,
     {A::ToggleDebugWindow, A::ToggleDebugWindow, A::ToggleDebugWindow, A::ToggleDebugWindow,
      A::ToggleDebugWindow, A::ToggleDebugWindow}},
};

TEST(KeymapTest, CoreVirtualKeyConstantsMatchWindowsHeaders) {
  EXPECT_EQ(vk::kBack, static_cast<uint32_t>(VK_BACK));
  EXPECT_EQ(vk::kReturn, static_cast<uint32_t>(VK_RETURN));
  EXPECT_EQ(vk::kKanji, static_cast<uint32_t>(VK_KANJI));
  EXPECT_EQ(vk::kOemAuto, static_cast<uint32_t>(VK_OEM_AUTO));
  EXPECT_EQ(vk::kEscape, static_cast<uint32_t>(VK_ESCAPE));
  EXPECT_EQ(vk::kConvert, static_cast<uint32_t>(VK_CONVERT));
  EXPECT_EQ(vk::kNonConvert, static_cast<uint32_t>(VK_NONCONVERT));
  EXPECT_EQ(vk::kOemAttn, static_cast<uint32_t>(VK_OEM_ATTN));
  EXPECT_EQ(vk::kSpace, static_cast<uint32_t>(VK_SPACE));
  EXPECT_EQ(vk::kEnd, static_cast<uint32_t>(VK_END));
  EXPECT_EQ(vk::kHome, static_cast<uint32_t>(VK_HOME));
  EXPECT_EQ(vk::kLeft, static_cast<uint32_t>(VK_LEFT));
  EXPECT_EQ(vk::kUp, static_cast<uint32_t>(VK_UP));
  EXPECT_EQ(vk::kRight, static_cast<uint32_t>(VK_RIGHT));
  EXPECT_EQ(vk::kDown, static_cast<uint32_t>(VK_DOWN));
  EXPECT_EQ(vk::kDelete, static_cast<uint32_t>(VK_DELETE));
  EXPECT_EQ(vk::k0, static_cast<uint32_t>('0'));
  EXPECT_EQ(vk::k9, static_cast<uint32_t>('9'));
  EXPECT_EQ(vk::kA, static_cast<uint32_t>('A'));
  EXPECT_EQ(vk::kZ, static_cast<uint32_t>('Z'));
  EXPECT_EQ(vk::kNumpad0, static_cast<uint32_t>(VK_NUMPAD0));
  EXPECT_EQ(vk::kNumpad9, static_cast<uint32_t>(VK_NUMPAD9));
  EXPECT_EQ(vk::kSubtract, static_cast<uint32_t>(VK_SUBTRACT));
  EXPECT_EQ(vk::kF10, static_cast<uint32_t>(VK_F10));
  EXPECT_EQ(vk::kOemComma, static_cast<uint32_t>(VK_OEM_COMMA));
  EXPECT_EQ(vk::kOemMinus, static_cast<uint32_t>(VK_OEM_MINUS));
  EXPECT_EQ(vk::kOemPeriod, static_cast<uint32_t>(VK_OEM_PERIOD));
  EXPECT_EQ(vk::kOem2, static_cast<uint32_t>(VK_OEM_2));
}

TEST(KeymapTest, EveryTableEntryMapsPerState) {
  for (const Entry& entry : kEntries) {
    for (size_t i = 0; i < std::size(kAllKinds); ++i) {
      EXPECT_EQ(Action(entry.key, entry.modifiers, kAllKinds[i]), entry.expected[i])
          << "vk=0x" << std::hex << entry.key << " mods=" << entry.modifiers << " kind=" << std::dec
          << i;
    }
  }
}

TEST(KeymapTest, EveryLetterIsInputOutsideReplaceSuggestion) {
  for (uint32_t key = 'A'; key <= 'Z'; ++key) {
    for (const K kind : kAllKinds) {
      const auto expected =
          kind == K::ReplaceSuggestion ? std::nullopt : std::optional<A>{A::Input};
      EXPECT_EQ(Action(key, 0, kind), expected) << static_cast<char>(key);
    }
  }
}

TEST(KeymapTest, DigitsCarryTheirIndexWhileSelecting) {
  for (uint32_t key = '1'; key <= '9'; ++key) {
    const auto event = MapUserAction(key, 0, K::Selecting);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->action, A::SelectByDigit);
    EXPECT_EQ(event->digit, static_cast<int>(key - '0'));
    EXPECT_EQ(event->codepoint, U'\0');
    EXPECT_FALSE(MapUserAction(key, 0, K::Composing).has_value());
    EXPECT_FALSE(MapUserAction(key, 0, K::Idle).has_value());
  }
}

TEST(KeymapTest, EventCarriesModifiersAndLeavesCodepointToTip) {
  const auto event = MapUserAction('K', kShift, K::Composing);
  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(*event, (UserActionEvent{A::Input, 0, kShift, 0}));
}

TEST(KeymapTest, AltAndWinChordsAlwaysPassThrough) {
  for (const uint32_t modifiers : {kAlt, kWin, kCtrl | kAlt, kShift | kWin}) {
    for (const K kind : kAllKinds) {
      for (const uint32_t key : {static_cast<uint32_t>('A'), static_cast<uint32_t>(VK_SPACE),
                                 static_cast<uint32_t>(VK_RETURN), static_cast<uint32_t>('U')}) {
        EXPECT_FALSE(MapUserAction(key, modifiers, kind).has_value());
      }
    }
  }
}

TEST(KeymapTest, UnlistedKeysPassThroughInEveryState) {
  for (const uint32_t key : {static_cast<uint32_t>(VK_TAB), static_cast<uint32_t>(VK_F1),
                             static_cast<uint32_t>(VK_PRIOR), static_cast<uint32_t>(VK_OEM_4)}) {
    for (const K kind : kAllKinds) {
      EXPECT_FALSE(MapUserAction(key, 0, kind).has_value()) << key;
    }
  }
}

}  // namespace
