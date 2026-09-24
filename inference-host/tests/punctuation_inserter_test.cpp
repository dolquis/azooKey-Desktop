#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>

#include "azookey/core/PlatformPaths.h"
#include "azookey/core/PunctuationRules.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/PunctuationInserter.h"

namespace {

azookey::ipc::LiveSegment Segment(std::string surface, std::string reading, double confidence = 0.9,
                                  uint8_t pos = 0, uint8_t head_pos = 0) {
  azookey::ipc::LiveSegment segment;
  segment.surface = std::move(surface);
  segment.reading = std::move(reading);
  segment.score = confidence;
  segment.pos = pos;
  segment.head_pos = head_pos;
  return segment;
}

TEST(PunctuationInserterTest, InsertsCommaAndPeriodWithSeparateReadinglessMarkers) {
  const auto rules = azookey::core::PunctuationRules::Default();
  const auto result = azookey::host::PunctuationInserter::Insert(
      {Segment("雨が降ったので", "あめがふったので"),
       Segment("出かけませんでした", "でかけませんでした")},
      rules, "ja", 0.5);
  EXPECT_EQ(result.surface, "雨が降ったので、出かけませんでした。");
  ASSERT_EQ(result.segments.size(), 4u);
  EXPECT_TRUE(result.segments[1].auto_punctuation);
  EXPECT_TRUE(result.segments[1].reading.empty());
  EXPECT_EQ(result.segments[1].start_char, 7u);
  EXPECT_EQ(result.segments[1].end_char, 8u);
  EXPECT_EQ(result.segments[2].reading, "でかけませんでした");
  EXPECT_TRUE(result.segments[3].auto_punctuation);
}

TEST(PunctuationInserterTest, SuppressesLowConfidenceAndAuxiliaryVerbBoundary) {
  const auto rules = azookey::core::PunctuationRules::Default();
  auto low = azookey::host::PunctuationInserter::Insert(
      {Segment("食べて", "たべて", 0.4), Segment("帰ります", "かえります")}, rules, "ja", 0.5);
  EXPECT_EQ(low.surface, "食べて帰ります。");

  auto auxiliary = azookey::host::PunctuationInserter::Insert(
      {Segment("食べて", "たべて"), Segment("いる", "いる")}, rules, "ja", 0.5);
  EXPECT_EQ(auxiliary.surface, "食べている");
}

TEST(PunctuationInserterTest, PosGuardAndStyle) {
  const auto rules = azookey::core::PunctuationRules::Default();
  auto grammatical = azookey::host::PunctuationInserter::Insert(
      {Segment("私が", "わたしが", 0.9, 3), Segment("明日行きます", "あしたいきます")}, rules,
      "fullwidth_latin", 0.5);
  EXPECT_EQ(grammatical.surface, "私が明日行きます．");

  auto conjunction = azookey::host::PunctuationInserter::Insert(
      {Segment("行ったが", "いったが", 0.9, 4), Segment("明日行きます", "あしたいきます")}, rules,
      "fullwidth_latin", 0.5);
  EXPECT_EQ(conjunction.surface, "行ったが，明日行きます．");
}

TEST(PunctuationInserterTest, SingleSegmentPeriodAndExistingPunctuation) {
  const auto rules = azookey::core::PunctuationRules::Default();
  auto result = azookey::host::PunctuationInserter::Insert(
      {Segment("今日はいい天気です", "きょうはいいてんきです")}, rules, "ja", 0.5);
  EXPECT_EQ(result.surface, "今日はいい天気です。");
  ASSERT_EQ(result.segments.size(), 2u);
  EXPECT_TRUE(result.segments[1].auto_punctuation);
  EXPECT_EQ(result.segments[1].start_char, 9u);
  EXPECT_EQ(result.segments[1].end_char, 10u);

  result = azookey::host::PunctuationInserter::Insert(
      {Segment("今日はいい天気です。", "きょうはいいてんきです")}, rules, "ja", 0.5);
  EXPECT_EQ(result.surface, "今日はいい天気です。");
}

TEST(PunctuationInserterTest, UnknownHeadAfterToSuppressesCommaButThoughtVerbAllowsIt) {
  const auto rules = azookey::core::PunctuationRules::Default();
  const auto nominal = azookey::host::PunctuationInserter::Insert(
      {Segment("私と", "わたしと"), Segment("彼が来る", "かれがくる")}, rules, "ja", 0.5);
  EXPECT_EQ(nominal.surface, "私と彼が来る");

  const auto quoted = azookey::host::PunctuationInserter::Insert(
      {Segment("行こうと", "いこうと"), Segment("思います", "おもいます")}, rules, "ja", 0.5);
  EXPECT_EQ(quoted.surface, "行こうと、思います。");
}

TEST(PunctuationInserterTest, DoesNotInsertBeforeClosingQuote) {
  const auto result = azookey::host::PunctuationInserter::Insert(
      {Segment("「やって", "やって"), Segment("」", "")},
      azookey::core::PunctuationRules::Default(), "ja", 0.5);
  EXPECT_EQ(result.surface, "「やって」");
  ASSERT_EQ(result.segments.size(), 2u);
}

TEST(PunctuationInserterTest, ApplyConfigPreservesPunctuationSettings) {
  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), nullptr,
                                        {});
  auto config = engine.config();
  config.dynamic_punctuation = true;
  config.segment_boundary_confidence = 0.8;
  config.punctuation_rules_path = "C:/rules/punctuation.tsv";
  engine.ApplyConfig(config);
  const auto applied = engine.config();
  EXPECT_TRUE(applied.dynamic_punctuation);
  EXPECT_DOUBLE_EQ(applied.segment_boundary_confidence, 0.8);
  EXPECT_EQ(applied.punctuation_rules_path, "C:/rules/punctuation.tsv");
}

TEST(PunctuationInserterTest, OversizedRulesFileFallsBackToBuiltIns) {
  const auto path = std::filesystem::temp_directory_path() / "azookey_oversized_punctuation.tsv";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out);
    out << "comma\tので\t0.0\n";
    out << std::string(1024 * 1024, '#');
  }
  const auto rules = azookey::host::PunctuationInserter::LoadRules(azookey::core::PathToUtf8(path));
  const auto result = azookey::host::PunctuationInserter::Insert(
      {Segment("雨なので", "あめなので"), Segment("帰ります", "かえります")}, rules, "ja", 0.5);
  EXPECT_EQ(result.surface, "雨なので、帰ります。");
  std::filesystem::remove(path);
}

}  // namespace
