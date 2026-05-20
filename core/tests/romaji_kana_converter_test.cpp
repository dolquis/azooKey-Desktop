#include <string>

#include <gtest/gtest.h>

#include "azookey/core/RomajiKanaConverter.h"

namespace {

std::string FeedAll(azookey::core::RomajiKanaConverter& converter,
                    const std::string& input) {
  std::string out;
  for (char c : input) {
    out += converter.Feed(c);
  }
  out += converter.Flush();
  return out;
}

}  // namespace

TEST(RomajiKanaConverterTest, FeedAndFlush) {
  azookey::core::RomajiKanaConverter converter;
  EXPECT_EQ(FeedAll(converter, "konnichiha"), "こんにちは");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "nani"), "なに");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "gakkou"), "がっこう");

  converter.Reset();
  EXPECT_EQ(FeedAll(converter, "konn"), "こん");
}

TEST(RomajiKanaConverterTest, Preview) {
  using azookey::core::RomajiKanaConverter;
  EXPECT_EQ(RomajiKanaConverter::Preview("k"), "k");
  EXPECT_EQ(RomajiKanaConverter::Preview("ka"), "か");
  EXPECT_EQ(RomajiKanaConverter::Preview("kan"), "かn");
  EXPECT_EQ(RomajiKanaConverter::Preview("na"), "な");
  EXPECT_EQ(RomajiKanaConverter::Preview("konn"), "こん");
  EXPECT_EQ(RomajiKanaConverter::Preview("konnichiha"), "こんにちは");
  EXPECT_EQ(RomajiKanaConverter::Preview("gakkou"), "がっこう");
}

TEST(RomajiKanaConverterTest, ConvertForCommit) {
  using azookey::core::RomajiKanaConverter;
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("kan"), "かん");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("na"), "な");
  EXPECT_EQ(RomajiKanaConverter::ConvertForCommit("konn"), "こん");
}
