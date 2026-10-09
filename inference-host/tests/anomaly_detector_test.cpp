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
  // The repeated quote maps to the first, then the second occurrence, and is
  // marked ambiguous by a capped confidence.
  EXPECT_EQ((*findings)[1].start, 13u);
  EXPECT_EQ((*findings)[2].start, 21u);
  EXPECT_DOUBLE_EQ((*findings)[1].confidence, 0.3);
  EXPECT_DOUBLE_EQ((*findings)[2].confidence, 0.3);
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

TEST(AnomalyDetectorTest, AmbiguousQuotesAreDroppedWhenShortAndDowngradedOtherwise) {
  const std::string text = "私は彼は来ると思う。彼は来ると思う。";
  const auto findings = ParseAnomalyFindings(
      text,
      R"([{"quote":"は","confidence":0.9},{"quote":"来ると思う","confidence":0.9},)"
      R"({"quote":"私は","confidence":0.9},{"quote":"clamped","confidence":2}])",
      10);
  ASSERT_TRUE(findings.has_value());
  ASSERT_EQ(findings->size(), 2u);
  // "私は" occurs once: placed with the model's confidence.
  EXPECT_EQ((*findings)[0].start, 0u);
  EXPECT_DOUBLE_EQ((*findings)[0].confidence, 0.9);
  // "来ると思う" occurs twice: kept, but no more than 0.3.
  EXPECT_EQ((*findings)[1].start, 4u);
  EXPECT_DOUBLE_EQ((*findings)[1].confidence, 0.3);
}

TEST(AnomalyDetectorTest, BoundsTheModelText) {
  const std::string text = "今日は晴れでした。";
  const std::string long_reason(600, 'r');
  const std::string long_suggestion(300, 's');
  const auto findings = ParseAnomalyFindings(
      text,
      R"([{"quote":"晴れでした","reason":")" + long_reason + R"(","suggestions":[")" +
          long_suggestion + R"(","晴れです"]},{"quote":"今日は","reason":")" +
          std::string(200, 'x') + std::string("あい") + std::string(400, 'y') + R"("}])",
      10);
  ASSERT_TRUE(findings.has_value());
  ASSERT_EQ(findings->size(), 2u);
  EXPECT_EQ((*findings)[1].reason.size(), 512u);
  EXPECT_EQ((*findings)[1].suggestions, std::vector<std::string>{"晴れです"});
  // Truncation keeps whole code points.
  EXPECT_LE((*findings)[0].reason.size(), 512u);
  EXPECT_EQ(Utf16Length((*findings)[0].reason), (*findings)[0].reason.size() - 4u);
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
