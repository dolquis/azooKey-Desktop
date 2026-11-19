#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "azookey/core/KanaInput.h"

namespace {

using azookey::core::ApplyVoicingMark;
using azookey::core::AsVoicingMark;
using azookey::core::LookupKanaJis;
using azookey::core::VoicingMark;

struct ExpectedKanaKey {
  uint8_t scan_code;
  std::string_view plain;
  std::string_view shifted;
};

// docs/input-style-spec.md section 4.2, written out independently of the implementation.
constexpr std::array<ExpectedKanaKey, 48> kExpectedKanaKeys = {{
    {0x02, "ぬ", "ぬ"}, {0x03, "ふ", "ふ"}, {0x04, "あ", "ぁ"}, {0x05, "う", "ぅ"},
    {0x06, "え", "ぇ"}, {0x07, "お", "ぉ"}, {0x08, "や", "ゃ"}, {0x09, "ゆ", "ゅ"},
    {0x0A, "よ", "ょ"}, {0x0B, "わ", "を"}, {0x0C, "ほ", "ほ"}, {0x0D, "へ", "へ"},
    {0x7D, "ー", "ー"}, {0x10, "た", "た"}, {0x11, "て", "て"}, {0x12, "い", "ぃ"},
    {0x13, "す", "す"}, {0x14, "か", "か"}, {0x15, "ん", "ん"}, {0x16, "な", "な"},
    {0x17, "に", "に"}, {0x18, "ら", "ら"}, {0x19, "せ", "せ"}, {0x1A, "゛", "゛"},
    {0x1B, "゜", "「"}, {0x1E, "ち", "ち"}, {0x1F, "と", "と"}, {0x20, "し", "し"},
    {0x21, "は", "は"}, {0x22, "き", "き"}, {0x23, "く", "く"}, {0x24, "ま", "ま"},
    {0x25, "の", "の"}, {0x26, "り", "り"}, {0x27, "れ", "れ"}, {0x28, "け", "け"},
    {0x2B, "む", "」"}, {0x2C, "つ", "っ"}, {0x2D, "さ", "さ"}, {0x2E, "そ", "そ"},
    {0x2F, "ひ", "ひ"}, {0x30, "こ", "こ"}, {0x31, "み", "み"}, {0x32, "も", "も"},
    {0x33, "ね", "、"}, {0x34, "る", "。"}, {0x35, "め", "・"}, {0x73, "ろ", "ろ"},
}};

bool IsTableKey(uint8_t scan_code) {
  for (const ExpectedKanaKey& key : kExpectedKanaKeys) {
    if (key.scan_code == scan_code) return true;
  }
  return false;
}

}  // namespace

TEST(KanaInputTest, EveryKeyMakesTheKanaOfSection42ForBothShiftStates) {
  for (const ExpectedKanaKey& key : kExpectedKanaKeys) {
    const auto plain = LookupKanaJis(key.scan_code, false, false);
    const auto shifted = LookupKanaJis(key.scan_code, false, true);
    ASSERT_TRUE(plain.has_value()) << "scan code " << int{key.scan_code};
    ASSERT_TRUE(shifted.has_value()) << "scan code " << int{key.scan_code};
    EXPECT_EQ(*plain, key.plain) << "scan code " << int{key.scan_code};
    EXPECT_EQ(*shifted, key.shifted) << "scan code " << int{key.scan_code};
  }
}

TEST(KanaInputTest, KeysOutsideTheTableHaveNoEntry) {
  for (int scan_code = 0; scan_code <= 0xff; ++scan_code) {
    if (IsTableKey(static_cast<uint8_t>(scan_code))) continue;
    EXPECT_FALSE(LookupKanaJis(static_cast<uint8_t>(scan_code), false, false).has_value())
        << "scan code " << scan_code;
    EXPECT_FALSE(LookupKanaJis(static_cast<uint8_t>(scan_code), false, true).has_value())
        << "scan code " << scan_code;
  }
}

// A numpad `/` has the scan code of the `/` key (0x35) plus the extended flag.
TEST(KanaInputTest, TheExtendedFlagReturnsNoEntry) {
  for (const ExpectedKanaKey& key : kExpectedKanaKeys) {
    EXPECT_FALSE(LookupKanaJis(key.scan_code, true, false).has_value())
        << "scan code " << int{key.scan_code};
    EXPECT_FALSE(LookupKanaJis(key.scan_code, true, true).has_value())
        << "scan code " << int{key.scan_code};
  }
}

// The ろ key (0x73) and the ¥ key (0x7D) give different kana: the table keys on the scan code,
// not on the character the layout makes.
TEST(KanaInputTest, SeparatesTheRoKeyFromTheYenKey) {
  EXPECT_EQ(LookupKanaJis(0x73, false, false), "ろ");
  EXPECT_EQ(LookupKanaJis(0x7D, false, false), "ー");
}

TEST(KanaInputTest, VoicingMarkKeysAreRecognized) {
  EXPECT_EQ(AsVoicingMark(*LookupKanaJis(0x1A, false, false)), VoicingMark::kDakuten);
  EXPECT_EQ(AsVoicingMark(*LookupKanaJis(0x1A, false, true)), VoicingMark::kDakuten);
  EXPECT_EQ(AsVoicingMark(*LookupKanaJis(0x1B, false, false)), VoicingMark::kHandakuten);
  EXPECT_FALSE(AsVoicingMark(*LookupKanaJis(0x1B, false, true)).has_value());
  EXPECT_FALSE(AsVoicingMark("か").has_value());
  EXPECT_FALSE(AsVoicingMark("").has_value());
}

// Section 4.3 / section 8 examples: the mark combines with the last character of the reading.
TEST(KanaInputTest, CombinesAMarkWithTheLastCharacter) {
  auto result = ApplyVoicingMark("か", VoicingMark::kDakuten);
  EXPECT_EQ(result.replace_bytes, 3u);
  EXPECT_EQ(result.text, "が");
  result = ApplyVoicingMark("う", VoicingMark::kDakuten);
  EXPECT_EQ(result.replace_bytes, 3u);
  EXPECT_EQ(result.text, "ゔ");
  result = ApplyVoicingMark("は", VoicingMark::kHandakuten);
  EXPECT_EQ(result.replace_bytes, 3u);
  EXPECT_EQ(result.text, "ぱ");
  // Only the last character is replaced.
  result = ApplyVoicingMark("あか", VoicingMark::kDakuten);
  EXPECT_EQ(result.replace_bytes, 3u);
  EXPECT_EQ(result.text, "が");
}

TEST(KanaInputTest, VoicesEveryCombinableRow) {
  const std::pair<std::string_view, std::string_view> dakuten[] = {
      {"う", "ゔ"}, {"か", "が"}, {"き", "ぎ"}, {"く", "ぐ"}, {"け", "げ"}, {"こ", "ご"},
      {"さ", "ざ"}, {"し", "じ"}, {"す", "ず"}, {"せ", "ぜ"}, {"そ", "ぞ"}, {"た", "だ"},
      {"ち", "ぢ"}, {"つ", "づ"}, {"て", "で"}, {"と", "ど"}, {"は", "ば"}, {"ひ", "び"},
      {"ふ", "ぶ"}, {"へ", "べ"}, {"ほ", "ぼ"},
  };
  for (const auto& [base, voiced] : dakuten) {
    const auto result = ApplyVoicingMark(base, VoicingMark::kDakuten);
    EXPECT_EQ(result.replace_bytes, base.size()) << base;
    EXPECT_EQ(result.text, voiced) << base;
  }
  const std::pair<std::string_view, std::string_view> handakuten[] = {
      {"は", "ぱ"}, {"ひ", "ぴ"}, {"ふ", "ぷ"}, {"へ", "ぺ"}, {"ほ", "ぽ"},
  };
  for (const auto& [base, semi_voiced] : handakuten) {
    const auto result = ApplyVoicingMark(base, VoicingMark::kHandakuten);
    EXPECT_EQ(result.replace_bytes, base.size()) << base;
    EXPECT_EQ(result.text, semi_voiced) << base;
  }
}

// Section 4.3 / section 8: a mark that cannot combine is added as a standalone mark.
TEST(KanaInputTest, AddsAStandaloneMarkWhenNothingCombines) {
  const std::string_view dakuten = "゛";
  const std::string_view handakuten = "゜";
  const auto standalone = [](const std::string& reading, VoicingMark mark) {
    return ApplyVoicingMark(reading, mark);
  };
  for (const char* reading : {"", "あ", "が", "ぱ", "ゔ", "な", "k", "。", "゛", "ぁ", "っ"}) {
    const auto result = standalone(reading, VoicingMark::kDakuten);
    EXPECT_EQ(result.replace_bytes, 0u) << reading;
    EXPECT_EQ(result.text, dakuten) << reading;
  }
  // The handakuten combines with the は row only.
  for (const char* reading : {"", "か", "ぱ", "ば", "う", "あ"}) {
    const auto result = standalone(reading, VoicingMark::kHandakuten);
    EXPECT_EQ(result.replace_bytes, 0u) << reading;
    EXPECT_EQ(result.text, handakuten) << reading;
  }
}

TEST(KanaInputTest, TruncatedUtf8BeforeTheCursorDoesNotCombine) {
  // Two of the three bytes of か.
  const std::string truncated = std::string("あ") + std::string("か").substr(0, 2);
  const auto result = ApplyVoicingMark(truncated, VoicingMark::kDakuten);
  EXPECT_EQ(result.replace_bytes, 0u);
  EXPECT_EQ(result.text, "゛");
}
