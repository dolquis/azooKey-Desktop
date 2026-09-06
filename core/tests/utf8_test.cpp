#include "azookey/core/Utf8.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <utility>
#include <vector>

TEST(Utf8Test, DecodesScalarBoundariesAndEmbeddedNull) {
  const std::vector<std::pair<std::string, char32_t>> cases{
      {std::string(1, '\0'), 0},     {"\x7f", 0x7f},           {"\xc2\x80", 0x80},
      {"\xdf\xbf", 0x7ff},           {"\xe0\xa0\x80", 0x800},  {"\xed\x9f\xbf", 0xd7ff},
      {"\xee\x80\x80", 0xe000},      {"\xef\xbf\xbf", 0xffff}, {"\xf0\x90\x80\x80", 0x10000},
      {"\xf4\x8f\xbf\xbf", 0x10ffff}};
  for (const auto& [bytes, expected] : cases) {
    const auto input = std::string("a") + bytes + "z";
    size_t offset = 1;
    char32_t codepoint = 0;
    ASSERT_TRUE(azookey::core::DecodeNextUtf8(input, offset, codepoint));
    EXPECT_EQ(codepoint, expected);
    EXPECT_EQ(offset, 1 + bytes.size());
    ASSERT_TRUE(azookey::core::DecodeNextUtf8(input, offset, codepoint));
    EXPECT_EQ(codepoint, U'z');
  }
}

TEST(Utf8Test, InvalidSequencesConsumeExactlyOneByte) {
  const std::vector<std::string> invalid{"\x80",
                                         "\xff",
                                         "\xf8\x88\x80\x80\x80",
                                         "\xc2",
                                         "\xe3\x81",
                                         "\xf0\x9f\x98",
                                         "\xc2x",
                                         "\xe3\x81x",
                                         "\xf0\x9f\x98x",
                                         "\xc0\x80",
                                         "\xc1\xbf",
                                         "\xe0\x80\x80",
                                         "\xf0\x80\x80\x80",
                                         "\xed\xa0\x80",
                                         "\xed\xbf\xbf",
                                         "\xf4\x90\x80\x80",
                                         "\xf7\xbf\xbf\xbf"};
  for (const auto& bytes : invalid) {
    const auto input = std::string("a") + bytes;
    size_t offset = 1;
    char32_t codepoint = U'z';
    EXPECT_FALSE(azookey::core::DecodeNextUtf8(input, offset, codepoint));
    EXPECT_EQ(offset, 2u);
    EXPECT_EQ(codepoint, static_cast<unsigned char>(bytes.front()));
  }
}

TEST(Utf8Test, EndOrOutOfRangeLeavesOutputsUnchanged) {
  for (const size_t start : {size_t{0}, size_t{1}, std::numeric_limits<size_t>::max()}) {
    size_t offset = start;
    char32_t codepoint = U'z';
    EXPECT_FALSE(azookey::core::DecodeNextUtf8({}, offset, codepoint));
    EXPECT_EQ(offset, start);
    EXPECT_EQ(codepoint, U'z');
  }
}

TEST(Utf8Test, SuffixPreservesCodepointBoundaries) {
  const std::string text = "A\xc2\xa2\xe3\x81\x82\xf0\x9f\x98\x80";
  const std::vector<std::string> expected{"",
                                          "\xf0\x9f\x98\x80",
                                          "\xe3\x81\x82\xf0\x9f\x98\x80",
                                          "\xc2\xa2\xe3\x81\x82\xf0\x9f\x98\x80",
                                          text,
                                          text};
  for (size_t count = 0; count < expected.size(); ++count) {
    EXPECT_EQ(azookey::core::TakeLastUtf8Codepoints(text, count), expected[count]);
  }
  EXPECT_TRUE(azookey::core::TakeLastUtf8Codepoints({}, 1).empty());
  EXPECT_EQ(azookey::core::TakeLastUtf8Codepoints(std::string("a\0z", 3), 2),
            std::string("\0z", 2));
}

TEST(Utf8Test, SuffixKeepsLegacyMalformedByteGroupsWithoutValidation) {
  for (const std::string bytes : {"\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe3\x81"}) {
    EXPECT_EQ(azookey::core::TakeLastUtf8Codepoints("a" + bytes, 1), bytes);
  }
  EXPECT_EQ(azookey::core::TakeLastUtf8Codepoints("a\x80\x80", 1), "a\x80\x80");
  EXPECT_EQ(azookey::core::TakeLastUtf8Codepoints("\x80\x80", 1), "\x80\x80");
  EXPECT_EQ(azookey::core::TakeLastUtf8Codepoints("\xc2x", 1), "x");
}
