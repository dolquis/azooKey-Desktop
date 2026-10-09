#include <gtest/gtest.h>

#include <string>

#include "azookey/host/AnomalyDetector.h"

namespace {

using azookey::host::AnomalyPersonaHint;
using azookey::host::ParseAnomalyFindings;
using azookey::host::Utf16Length;

TEST(AnomalyDetectorTest, Utf16LengthCountsSurrogatePairsAsTwoUnits) {
  EXPECT_EQ(Utf16Length(""), 0u);
  EXPECT_EQ(Utf16Length("abc"), 3u);
  EXPECT_EQ(Utf16Length("日本語"), 3u);
  EXPECT_EQ(Utf16Length("😀a"), 3u);
  // An invalid byte is one unit and does not stall the scan.
  EXPECT_EQ(Utf16Length(std::string("\xFF", 1) + "a"), 2u);
}

TEST(AnomalyDetectorTest, PlacesQuotesAsUtf16OffsetsInOrder) {
  const std::string text = "😀今日は晴れです。明日は雨でした。明日は雨でした。";
  const auto findings = ParseAnomalyFindings(text,
                                             R"([
        {"quote":"雨でした","reason":"時制","suggestions":["雨です","雨でした"],"confidence":0.8},
        {"quote":"雨でした","reason":"時制","suggestions":[],"confidence":2},
        {"quote":"晴れです","reason":"r","suggestions":["晴れでした"]}
      ])",
                                             10);
  ASSERT_TRUE(findings.has_value());
  ASSERT_EQ(findings->size(), 3u);
  // "😀" is two UTF-16 units, so "晴れです" starts at 2 + 3.
  EXPECT_EQ((*findings)[0].start, 5u);
  EXPECT_EQ((*findings)[0].length, 4u);
  EXPECT_DOUBLE_EQ((*findings)[0].confidence, 0.5);  // missing -> 0.5
  // The repeated quote maps to the first, then the second occurrence.
  EXPECT_EQ((*findings)[1].start, 13u);
  EXPECT_EQ((*findings)[2].start, 21u);
  EXPECT_DOUBLE_EQ((*findings)[2].confidence, 1.0);  // clamped
  // A suggestion equal to the quote is not a correction.
  EXPECT_EQ((*findings)[1].suggestions, std::vector<std::string>{"雨です"});
}

TEST(AnomalyDetectorTest, DropsUnplaceableItemsAndCapsTheCount) {
  const std::string text = "あいうえお";
  const auto findings = ParseAnomalyFindings(
      text,
      R"([{"quote":"かき"},{"quote":""},"x",{"reason":"no quote"},{"quote":"あ"},{"quote":"い"},{"quote":"う"}])",
      2);
  ASSERT_TRUE(findings.has_value());
  ASSERT_EQ(findings->size(), 2u);
  EXPECT_EQ((*findings)[0].start, 0u);
  EXPECT_EQ((*findings)[1].start, 1u);

  EXPECT_FALSE(ParseAnomalyFindings(text, R"({"quote":"あ"})", 5).has_value());
  EXPECT_FALSE(ParseAnomalyFindings(text, "not json", 5).has_value());
  const auto none = ParseAnomalyFindings(text, "[]", 5);
  ASSERT_TRUE(none.has_value());
  EXPECT_TRUE(none->empty());
}

TEST(AnomalyDetectorTest, ReportsTheSameSpanOnceButKeepsOverlaps) {
  const std::string text = "明日は雨でした。";
  const auto findings =
      ParseAnomalyFindings(text, R"([{"quote":"雨"},{"quote":"雨"},{"quote":"雨でした"}])", 10);
  ASSERT_TRUE(findings.has_value());
  ASSERT_EQ(findings->size(), 2u);
  EXPECT_EQ((*findings)[0].start, 3u);
  EXPECT_EQ((*findings)[0].length, 1u);
  EXPECT_EQ((*findings)[1].start, 3u);
  EXPECT_EQ((*findings)[1].length, 4u);
}

TEST(AnomalyDetectorTest, PersonaHintOnlyWithSamples) {
  EXPECT_TRUE(AnomalyPersonaHint(std::nullopt).empty());
  azookey::learning::Persona persona;
  EXPECT_TRUE(AnomalyPersonaHint(persona).empty());
  persona.sample_count = 10;
  persona.polite_ratio = 0.25;
  const auto hint = AnomalyPersonaHint(persona);
  EXPECT_NE(hint.find("polite 0.25"), std::string::npos);
}

}  // namespace
