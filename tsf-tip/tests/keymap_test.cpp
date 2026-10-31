#include <gtest/gtest.h>
#include <windows.h>
// windows.h does not pull in the VK_DBE_* keys.
#include <ime.h>

#include <iterator>
#include <optional>
#include <vector>

#include "azookey/core/UserActionMap.h"
#include "azookey/tsf/OpenCloseKey.h"

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
    // Digits (DEV-1811): the reading takes them, Selecting 1-9 select and 0
    // commits, Idle leaves them to the app (the number rewriter may claim them).
    {'1', 0, {kNone, A::Input, A::Input, A::SelectByDigit, kNone, A::Input}},
    {'0', 0, {kNone, A::Input, A::Input, A::Input, kNone, A::Input}},
    {VK_NUMPAD5, 0, {kNone, A::Input, A::Input, A::SelectByDigit, kNone, A::Input}},
    {VK_NUMPAD0, 0, {kNone, A::Input, A::Input, A::Input, kNone, A::Input}},
    // Shift+digits type the layout's symbol (Shift+1 is ！, DEV-1670) and
    // never select.
    {'1', kShift, {A::Input, A::Input, A::Input, A::Input, kNone, A::Input}},
    {'9', kShift, {A::Input, A::Input, A::Input, A::Input, kNone, A::Input}},
    {'0', kShift, {A::Input, A::Input, A::Input, A::Input, kNone, A::Input}},
    // Symbol keys start a reading from Idle; the TIP passes through a
    // character outside the full-width table (spec §1.5.3).
    {VK_OEM_MINUS, 0, {A::Input, A::Input, A::Input, A::Input, kNone, A::Input}},
    {VK_OEM_MINUS, kShift, {A::Input, A::Input, A::Input, A::Input, kNone, A::Input}},
    {VK_OEM_COMMA, 0, {A::Input, A::Input, A::Input, A::Input, kNone, A::Input}},
    {VK_OEM_PERIOD, 0, {A::Input, A::Input, A::Input, A::Input, kNone, A::Input}},
    {VK_OEM_2, 0, {A::Input, A::Input, A::Input, A::Input, kNone, A::Input}},
    {VK_OEM_2, kShift, {A::Input, A::Input, A::Input, A::Input, kNone, A::Input}},
    {VK_OEM_1, 0, {A::Input, A::Input, A::Input, A::Input, kNone, kNone}},
    {VK_OEM_PLUS, kShift, {A::Input, A::Input, A::Input, A::Input, kNone, kNone}},
    {VK_OEM_3, 0, {A::Input, A::Input, A::Input, A::Input, kNone, kNone}},
    {VK_OEM_4, 0, {A::Input, A::Input, A::Input, A::Input, kNone, kNone}},
    {VK_OEM_5, kShift, {A::Input, A::Input, A::Input, A::Input, kNone, kNone}},
    {VK_OEM_6, 0, {A::Input, A::Input, A::Input, A::Input, kNone, kNone}},
    {VK_OEM_7, 0, {A::Input, A::Input, A::Input, A::Input, kNone, kNone}},
    {VK_OEM_8, 0, {A::Input, A::Input, A::Input, A::Input, kNone, kNone}},
    {VK_OEM_102, 0, {A::Input, A::Input, A::Input, A::Input, kNone, kNone}},
    // Numpad symbols join a reading but never start one.
    {VK_SUBTRACT, 0, {kNone, A::Input, A::Input, A::Input, kNone, A::Input}},
    {VK_ADD, 0, {kNone, A::Input, A::Input, A::Input, kNone, kNone}},
    {VK_MULTIPLY, 0, {kNone, A::Input, A::Input, A::Input, kNone, kNone}},
    {VK_DECIMAL, 0, {kNone, A::Input, A::Input, A::Input, kNone, kNone}},
    {VK_DIVIDE, 0, {kNone, A::Input, A::Input, A::Input, kNone, kNone}},
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
    // The TIP's Unicode mode (spec §6) relies on these reaching the hex buffer.
    {'U', kShift, {A::Input, A::Input, A::Input, A::Input, kNone, A::Input}},
    {VK_NUMPAD9, 0, {kNone, A::Input, A::Input, A::SelectByDigit, kNone, A::Input}},
    {VK_BACK, kCtrl, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {VK_F10, kShift, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {VK_BACK, kCtrl | kShift, {A::Forget, kNone, kNone, kNone, kNone, kNone}},
    {VK_KANJI,
     0,
     {A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku,
      A::ToggleHankaku}},
    {VK_OEM_AUTO,
     0,
     {A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku,
      A::ToggleHankaku}},
    {VK_OEM_ENLW,
     0,
     {A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku, A::ToggleHankaku,
      A::ToggleHankaku}},
    {VK_NONCONVERT,
     0,
     {A::ToggleHiraKata, A::ToggleHiraKata, A::ToggleHiraKata, A::ToggleHiraKata, A::ToggleHiraKata,
      A::ToggleHiraKata}},
    {VK_CONVERT,
     0,
     {kNone, A::StartConversion, A::StartConversion, A::NextCandidate, kNone, kNone}},
    {VK_OEM_ATTN,
     0,
     {A::ToggleAlnum, A::ToggleAlnum, A::ToggleAlnum, A::ToggleAlnum, A::ToggleAlnum,
      A::ToggleAlnum}},
    // F6-F10 commit a reading in one form (tsf-deep-integration-spec §3.6, DEV-1812).
    {VK_F6,
     0,
     {kNone, A::CommitHiragana, A::CommitHiragana, A::CommitHiragana, kNone, A::CommitHiragana}},
    {VK_F7,
     0,
     {kNone, A::CommitKatakana, A::CommitKatakana, A::CommitKatakana, kNone, A::CommitKatakana}},
    {VK_F8,
     0,
     {kNone, A::CommitHalfwidthKatakana, A::CommitHalfwidthKatakana, A::CommitHalfwidthKatakana,
      kNone, A::CommitHalfwidthKatakana}},
    {VK_F9,
     0,
     {kNone, A::CommitFullwidthRoman, A::CommitFullwidthRoman, A::CommitFullwidthRoman, kNone,
      A::CommitFullwidthRoman}},
    {VK_F10,
     0,
     {A::ToggleDebugWindow, A::CommitHalfwidthRoman, A::CommitHalfwidthRoman,
      A::CommitHalfwidthRoman, A::ToggleDebugWindow, A::CommitHalfwidthRoman}},
    {VK_F6, kShift, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {VK_F7, kCtrl, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {VK_F9, kCtrl | kShift, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {VK_F11, 0, {kNone, kNone, kNone, kNone, kNone, kNone}},
};

TEST(KeymapTest, CoreVirtualKeyConstantsMatchWindowsHeaders) {
  EXPECT_EQ(vk::kBack, static_cast<uint32_t>(VK_BACK));
  EXPECT_EQ(vk::kReturn, static_cast<uint32_t>(VK_RETURN));
  EXPECT_EQ(vk::kKanji, static_cast<uint32_t>(VK_KANJI));
  EXPECT_EQ(vk::kImeOn, static_cast<uint32_t>(VK_IME_ON));
  EXPECT_EQ(vk::kImeOff, static_cast<uint32_t>(VK_IME_OFF));
  EXPECT_EQ(vk::kOemAuto, static_cast<uint32_t>(VK_OEM_AUTO));
  EXPECT_EQ(vk::kOemEnlw, static_cast<uint32_t>(VK_OEM_ENLW));
  EXPECT_EQ(vk::kEscape, static_cast<uint32_t>(VK_ESCAPE));
  EXPECT_EQ(vk::kConvert, static_cast<uint32_t>(VK_CONVERT));
  EXPECT_EQ(vk::kNonConvert, static_cast<uint32_t>(VK_NONCONVERT));
  EXPECT_EQ(vk::kOemAttn, static_cast<uint32_t>(VK_OEM_ATTN));
  EXPECT_EQ(vk::kDbeHiragana, static_cast<uint32_t>(VK_DBE_HIRAGANA));
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
  EXPECT_EQ(vk::kF6, static_cast<uint32_t>(VK_F6));
  EXPECT_EQ(vk::kF7, static_cast<uint32_t>(VK_F7));
  EXPECT_EQ(vk::kF8, static_cast<uint32_t>(VK_F8));
  EXPECT_EQ(vk::kF9, static_cast<uint32_t>(VK_F9));
  EXPECT_EQ(vk::kF10, static_cast<uint32_t>(VK_F10));
  EXPECT_EQ(vk::kOemComma, static_cast<uint32_t>(VK_OEM_COMMA));
  EXPECT_EQ(vk::kOemMinus, static_cast<uint32_t>(VK_OEM_MINUS));
  EXPECT_EQ(vk::kOemPeriod, static_cast<uint32_t>(VK_OEM_PERIOD));
  EXPECT_EQ(vk::kOem2, static_cast<uint32_t>(VK_OEM_2));
  EXPECT_EQ(vk::kOem1, static_cast<uint32_t>(VK_OEM_1));
  EXPECT_EQ(vk::kOemPlus, static_cast<uint32_t>(VK_OEM_PLUS));
  EXPECT_EQ(vk::kOem3, static_cast<uint32_t>(VK_OEM_3));
  EXPECT_EQ(vk::kOem4, static_cast<uint32_t>(VK_OEM_4));
  EXPECT_EQ(vk::kOem8, static_cast<uint32_t>(VK_OEM_8));
  EXPECT_EQ(vk::kOem102, static_cast<uint32_t>(VK_OEM_102));
  EXPECT_EQ(vk::kMultiply, static_cast<uint32_t>(VK_MULTIPLY));
  EXPECT_EQ(vk::kAdd, static_cast<uint32_t>(VK_ADD));
  EXPECT_EQ(vk::kDecimal, static_cast<uint32_t>(VK_DECIMAL));
  EXPECT_EQ(vk::kDivide, static_cast<uint32_t>(VK_DIVIDE));
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
    // The numpad digits are candidate numbers too (spec §1.4).
    const auto numpad = MapUserAction(VK_NUMPAD0 + (key - '0'), 0, K::Selecting);
    ASSERT_TRUE(numpad.has_value());
    EXPECT_EQ(numpad->action, A::SelectByDigit);
    EXPECT_EQ(numpad->digit, static_cast<int>(key - '0'));
    EXPECT_EQ(Action(key, 0, K::Composing), A::Input);
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
                                 static_cast<uint32_t>(VK_RETURN), static_cast<uint32_t>('U'),
                                 static_cast<uint32_t>(VK_F7), static_cast<uint32_t>(VK_F10)}) {
        EXPECT_FALSE(MapUserAction(key, modifiers, kind).has_value());
      }
    }
  }
}

TEST(KeymapTest, UnlistedKeysPassThroughInEveryState) {
  // VK_IME_ON / VK_IME_OFF are decided by the TIP before the core map (OpenCloseKey.h).
  for (const uint32_t key : {static_cast<uint32_t>(VK_TAB), static_cast<uint32_t>(VK_F1),
                             static_cast<uint32_t>(VK_PRIOR), static_cast<uint32_t>(VK_OEM_CLEAR),
                             static_cast<uint32_t>(VK_IME_ON), static_cast<uint32_t>(VK_IME_OFF)}) {
    for (const K kind : kAllKinds) {
      EXPECT_FALSE(MapUserAction(key, 0, kind).has_value()) << key;
    }
  }
}

using azookey::tsf::ClassifyOpenCloseKey;
using azookey::tsf::ConvertKeysAction;
using azookey::tsf::OpenCloseKeyAction;

TEST(OpenCloseKeyTest, ImeOnAndOffKeysAreFixedOnAndOffWithEitherSetting) {
  for (const auto setting : {ConvertKeysAction::Default, ConvertKeysAction::ImeOnOff}) {
    EXPECT_EQ(ClassifyOpenCloseKey(VK_IME_ON, 0, setting), OpenCloseKeyAction::Open);
    EXPECT_EQ(ClassifyOpenCloseKey(VK_IME_OFF, 0, setting), OpenCloseKeyAction::Close);
    for (const uint32_t key : {static_cast<uint32_t>(VK_KANJI), static_cast<uint32_t>(VK_OEM_AUTO),
                               static_cast<uint32_t>(VK_OEM_ENLW)}) {
      EXPECT_EQ(ClassifyOpenCloseKey(key, 0, setting), OpenCloseKeyAction::Toggle) << key;
    }
    for (const uint32_t key : {static_cast<uint32_t>('A'), static_cast<uint32_t>(VK_OEM_ATTN),
                               static_cast<uint32_t>(VK_SPACE), static_cast<uint32_t>(VK_F10)}) {
      EXPECT_EQ(ClassifyOpenCloseKey(key, 0, setting), OpenCloseKeyAction::None) << key;
    }
  }
}

TEST(OpenCloseKeyTest, ConvertKeysOpenAndCloseOnlyUnderImeOnOff) {
  EXPECT_EQ(ClassifyOpenCloseKey(VK_CONVERT, 0, ConvertKeysAction::Default),
            OpenCloseKeyAction::None);
  EXPECT_EQ(ClassifyOpenCloseKey(VK_NONCONVERT, 0, ConvertKeysAction::Default),
            OpenCloseKeyAction::None);
  EXPECT_EQ(ClassifyOpenCloseKey(VK_CONVERT, 0, ConvertKeysAction::ImeOnOff),
            OpenCloseKeyAction::Open);
  EXPECT_EQ(ClassifyOpenCloseKey(VK_NONCONVERT, 0, ConvertKeysAction::ImeOnOff),
            OpenCloseKeyAction::Close);
}

TEST(OpenCloseKeyTest, HiraganaKeyOpensLikeImeOnAndKatakanaKeyIsNotHandled) {
  for (const auto setting : {ConvertKeysAction::Default, ConvertKeysAction::ImeOnOff}) {
    EXPECT_EQ(ClassifyOpenCloseKey(VK_DBE_HIRAGANA, 0, setting), OpenCloseKeyAction::Open);
    // No katakana input mode exists, so the Shift-ed kana key reaches the app (spec §3.1).
    EXPECT_EQ(ClassifyOpenCloseKey(VK_DBE_KATAKANA, 0, setting), OpenCloseKeyAction::None);
  }
  // The open/close keys are classified before the map, which leaves both kana keys unmapped.
  for (const auto kind : kAllKinds) {
    EXPECT_FALSE(MapUserAction(VK_DBE_HIRAGANA, 0, kind).has_value());
    EXPECT_FALSE(MapUserAction(VK_DBE_KATAKANA, 0, kind).has_value());
  }
}

TEST(OpenCloseKeyTest, ModifiedKeysAreNotOpenCloseKeys) {
  for (const uint32_t modifiers : {kShift, kCtrl, kAlt, kWin, kCtrl | kShift}) {
    for (const uint32_t key :
         {static_cast<uint32_t>(VK_IME_ON), static_cast<uint32_t>(VK_IME_OFF),
          static_cast<uint32_t>(VK_DBE_HIRAGANA), static_cast<uint32_t>(VK_KANJI),
          static_cast<uint32_t>(VK_CONVERT), static_cast<uint32_t>(VK_NONCONVERT)}) {
      EXPECT_EQ(ClassifyOpenCloseKey(key, modifiers, ConvertKeysAction::ImeOnOff),
                OpenCloseKeyAction::None)
          << key << " " << modifiers;
    }
  }
}

}  // namespace
