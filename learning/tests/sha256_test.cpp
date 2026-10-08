#include "azookey/learning/Sha256.h"

#include <gtest/gtest.h>

#include <string>

using azookey::learning::Sha256;
using azookey::learning::Sha256Hex;

TEST(Sha256Test, EmptyInputMatchesNistVector) {
  EXPECT_EQ(Sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256Test, AbcMatchesNistVector) {
  EXPECT_EQ(Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256Test, TwoBlockMessageMatchesNistVector) {
  EXPECT_EQ(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256Test, OneMillionAMatchesNistVector) {
  EXPECT_EQ(Sha256Hex(std::string(1000000, 'a')),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// 55 bytes is the longest input whose padding fits one block, 56 forces a
// second padding block, and 63 / 64 straddle the block boundary itself.
TEST(Sha256Test, PaddingBoundaryLengthsMatchReferenceDigests) {
  EXPECT_EQ(Sha256Hex(std::string(55, 'a')),
            "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  EXPECT_EQ(Sha256Hex(std::string(56, 'a')),
            "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  EXPECT_EQ(Sha256Hex(std::string(63, 'a')),
            "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
  EXPECT_EQ(Sha256Hex(std::string(64, 'a')),
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST(Sha256Test, HexIsLowercaseAndMatchesRawDigest) {
  const auto digest = Sha256("abc");
  const std::string hex = Sha256Hex("abc");
  ASSERT_EQ(hex.size(), 64u);
  EXPECT_EQ(digest[0], 0xba);
  EXPECT_EQ(digest[31], 0xad);
  for (char c : hex) EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << c;
}

TEST(Sha256Test, EmbeddedNulBytesAreHashed) {
  const std::string with_nul("a\0b", 3);
  EXPECT_NE(Sha256Hex(with_nul), Sha256Hex("ab"));
  EXPECT_EQ(Sha256Hex(with_nul), Sha256Hex(std::string_view("a\0b", 3)));
}
