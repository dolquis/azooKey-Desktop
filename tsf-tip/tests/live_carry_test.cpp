#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "azookey/tsf/LiveCarry.h"

namespace {

using azookey::tsf::BuildLivePieces;
using azookey::tsf::CarryLiveText;

azookey::ipc::LiveSegment Segment(std::string surface, std::string reading,
                                  bool auto_punctuation = false) {
  azookey::ipc::LiveSegment segment;
  segment.surface = std::move(surface);
  segment.reading = std::move(reading);
  segment.auto_punctuation = auto_punctuation;
  return segment;
}

// The text shown for `typed` from one answer: the kept start and the rest as typed.
std::string Shown(const std::string& reading, const std::string& surface, const std::string& typed,
                  const std::vector<azookey::ipc::LiveSegment>& segments = {},
                  bool auto_punctuation = false) {
  const auto carried =
      CarryLiveText(BuildLivePieces(reading, surface, segments, auto_punctuation), typed);
  if (carried.reading_bytes == 0) return {};
  return carried.surface + typed.substr(carried.reading_bytes);
}

}  // namespace

// DEV-1806 (docs/legacy-parity-spec.md section 2.3.1).
TEST(TsfTipLiveCarryPiecesTest, LongerReadingKeepsTheWholeAnswer) {
  EXPECT_EQ(Shown("きょうはいいてんき", "今日はいい天気", "きょうはいいてんきが"),
            "今日はいい天気が");
  EXPECT_EQ(Shown("かき", "柿", "かきく"), "柿く");
}

TEST(TsfTipLiveCarryPiecesTest, ShorterReadingKeepsTheStartUpToAKana) {
  EXPECT_EQ(Shown("きょうはいいてんき", "今日はいい天気", "きょうはいいてん"), "今日はいいてん");
  EXPECT_EQ(Shown("きょうはいいてんき", "今日はいい天気", "きょうは"), "今日は");
  // The kanji before the first kana needs that kana to end it.
  EXPECT_EQ(Shown("きょうはいいてんき", "今日はいい天気", "きょう"), "");
}

TEST(TsfTipLiveCarryPiecesTest, AStemWithoutItsKanaIsNotShown) {
  EXPECT_EQ(Shown("かく", "書く", "か"), "");
  EXPECT_EQ(Shown("かき", "柿", "か"), "");
}

TEST(TsfTipLiveCarryPiecesTest, KatakanaAndLongVowelsAreKana) {
  EXPECT_EQ(Shown("てれびをみる", "テレビを見る", "てれびをみ"), "テレビをみ");
  EXPECT_EQ(Shown("こーひーをのむ", "コーヒーを飲む", "こーひー"), "コーヒー");
}

TEST(TsfTipLiveCarryPiecesTest, AnAnswerThatSplitsMoreThanOneWayIsKeptWhole) {
  // Either 母 can read は or はは: the split is not the only one, so only the
  // whole answer is kept.
  EXPECT_EQ(Shown("はははは", "母は母", "はははは"), "母は母");
  EXPECT_EQ(Shown("はははは", "母は母", "ははは"), "");
}

TEST(TsfTipLiveCarryPiecesTest, SmallKeAndKaJoinTheRunAroundThem) {
  // "ヶ" reads か, け or こ, so it is part of the run "3ヶ月", not a kana to match.
  EXPECT_EQ(Shown("さんかげつのたび", "3ヶ月の旅", "さんかげつのたびに"), "3ヶ月の旅に");
  EXPECT_EQ(Shown("さんかげつのたび", "3ヶ月の旅", "さんかげつの"), "3ヶ月の");
  EXPECT_EQ(Shown("ありがとう", "有難う", "ありがと"), "");
}

TEST(TsfTipLiveCarryPiecesTest, SegmentsAreCutAtTheirEnds) {
  const std::vector segments = {Segment("蚊", "か"), Segment("木", "き")};
  EXPECT_EQ(Shown("かき", "蚊木", "か", segments), "蚊");
  EXPECT_EQ(Shown("かき", "蚊木", "かきの", segments), "蚊木の");
}

TEST(TsfTipLiveCarryPiecesTest, AutomaticMarksStayOnlyBetweenKeptPieces) {
  const std::vector segments = {Segment("今日は", "きょうは"), Segment("、", "", true),
                                Segment("晴れ", "はれ"), Segment("。", "", true)};
  const std::string surface = "今日は、晴れ。";
  EXPECT_EQ(Shown("きょうははれ", surface, "きょうははれだ", segments, true), "今日は、晴れだ");
  EXPECT_EQ(Shown("きょうははれ", surface, "きょうはね", segments, true), "今日はね");
}

TEST(TsfTipLiveCarryPiecesTest, PunctuatedAnswerWithoutUsableSegmentsGivesNoPieces) {
  EXPECT_TRUE(BuildLivePieces("か", "蚊。", {}, true).empty());
  EXPECT_TRUE(BuildLivePieces("か", "蚊。", {Segment("蚊", "け")}, true).empty());
  EXPECT_FALSE(BuildLivePieces("か", "蚊", {}, false).empty());
  EXPECT_TRUE(BuildLivePieces("", "蚊", {}, false).empty());
}

TEST(TsfTipLiveCarryPiecesTest, LongSegmentsAreKeptWhole) {
  std::string reading = "はなを";
  std::string surface = "花を";
  for (int i = 0; i < 61; ++i) {
    reading += "あ";
    surface += "あ";
  }
  // 64 reading characters still split; 65 do not.
  EXPECT_EQ(Shown(reading, surface, "はなを"), "花を");
  EXPECT_EQ(Shown(reading + "あ", surface + "あ", "はなを"), "");
  EXPECT_EQ(Shown(reading + "あ", surface + "あ", reading + "あい"), surface + "あい");
}

TEST(TsfTipLiveCarryPiecesTest, InvalidUtf8StaysInARun) {
  EXPECT_EQ(Shown("か\xff", "蚊\xff", "か\xffき"), "蚊\xffき");
  EXPECT_EQ(Shown("か\xff", "蚊\xff", "か"), "");
}
