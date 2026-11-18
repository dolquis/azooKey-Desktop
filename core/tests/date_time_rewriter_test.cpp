#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "azookey/core/DateTimeRewriter.h"

namespace {

using azookey::core::ExpandDateTimeCandidates;
using azookey::core::LocalDateTime;

// 2026-11-17 (Tuesday) 14:05, the spec §20.4 example.
constexpr LocalDateTime kSpecExampleTime{2026, 11, 17, 14, 5};

std::vector<std::string> DateTimeSurfaces(const std::string& reading, const LocalDateTime& now) {
  std::vector<std::string> surfaces;
  for (const auto& candidate : ExpandDateTimeCandidates(reading, now))
    surfaces.push_back(candidate.surface);
  return surfaces;
}

std::string DateTimeSeireki(const std::string& reading, const LocalDateTime& now) {
  const auto candidates = ExpandDateTimeCandidates(reading, now);
  return candidates.empty() ? std::string() : candidates.front().surface;
}

}  // namespace

TEST(DateTimeRewriterTest, DayCandidatesFollowTheSpecTableOrderWithAnnotations) {
  const auto candidates = ExpandDateTimeCandidates("きょう", kSpecExampleTime);

  const std::vector<std::pair<std::string, std::string>> expected = {
      {"2026年11月17日", "西暦"},           {"令和8年11月17日", "和暦"},
      {"2026/11/17", "西暦（スラッシュ）"}, {"2026-11-17", "ISO 8601"},
      {"11月17日（火）", "月日と曜日"},     {"火曜日", "曜日"},
  };
  ASSERT_EQ(candidates.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(candidates[i].surface, expected[i].first) << i;
    EXPECT_EQ(candidates[i].description, expected[i].second) << i;
    EXPECT_EQ(candidates[i].reading, "きょう");
    EXPECT_EQ(candidates[i].source, azookey::core::CandidateSource::Heuristic);
    EXPECT_EQ(candidates[i].debug_info.rfind("date-rewriter:", 0), 0u) << candidates[i].debug_info;
  }
}

TEST(DateTimeRewriterTest, PadsOnlySlashAndIsoMonthAndDay) {
  EXPECT_EQ(DateTimeSurfaces("きょう", {2026, 3, 7, 9, 0}),
            (std::vector<std::string>{"2026年3月7日", "令和8年3月7日", "2026/03/07", "2026-03-07",
                                      "3月7日（土）", "土曜日"}));
}

TEST(DateTimeRewriterTest, EveryTableReadingProducesItsKindAndOffset) {
  const std::vector<std::pair<std::string, std::string>> days = {
      {"きょう", "2026年11月17日"},       {"ほんじつ", "2026年11月17日"},
      {"あした", "2026年11月18日"},       {"あす", "2026年11月18日"},
      {"みょうにち", "2026年11月18日"},   {"あさって", "2026年11月19日"},
      {"みょうごにち", "2026年11月19日"}, {"きのう", "2026年11月16日"},
      {"さくじつ", "2026年11月16日"},     {"おととい", "2026年11月15日"},
      {"いっさくじつ", "2026年11月15日"},
  };
  for (const auto& [reading, seireki] : days) {
    EXPECT_EQ(ExpandDateTimeCandidates(reading, kSpecExampleTime).size(), 6u) << reading;
    EXPECT_EQ(DateTimeSeireki(reading, kSpecExampleTime), seireki) << reading;
  }

  EXPECT_EQ(DateTimeSurfaces("ことし", kSpecExampleTime),
            (std::vector<std::string>{"2026年", "令和8年"}));
  EXPECT_EQ(DateTimeSurfaces("らいねん", kSpecExampleTime),
            (std::vector<std::string>{"2027年", "令和9年"}));
  EXPECT_EQ(DateTimeSurfaces("きょねん", kSpecExampleTime),
            (std::vector<std::string>{"2025年", "令和7年"}));
  EXPECT_EQ(DateTimeSurfaces("さくねん", kSpecExampleTime),
            (std::vector<std::string>{"2025年", "令和7年"}));
  EXPECT_EQ(DateTimeSurfaces("いま", kSpecExampleTime),
            (std::vector<std::string>{"14:05", "14時5分", "午後2時5分"}));
}

TEST(DateTimeRewriterTest, OnlyExactReadingsMatch) {
  for (const std::string reading :
       {"", "きょ", "きょうは", "きょうう", "キョウ", "今日", "ま", "いまは", "こんげつ",
        "らいしゅう", "げつようび", "1+2=", "kyou"}) {
    EXPECT_TRUE(ExpandDateTimeCandidates(reading, kSpecExampleTime).empty()) << reading;
  }
}

TEST(DateTimeRewriterTest, DayOffsetsCrossMonthEndsYearEndsAndLeapDays) {
  EXPECT_EQ(DateTimeSeireki("あした", {2026, 1, 31, 0, 0}), "2026年2月1日");
  EXPECT_EQ(DateTimeSeireki("きのう", {2026, 3, 1, 0, 0}), "2026年2月28日");
  EXPECT_EQ(DateTimeSeireki("あした", {2026, 12, 31, 23, 59}), "2027年1月1日");
  EXPECT_EQ(DateTimeSeireki("おととい", {2027, 1, 1, 0, 0}), "2026年12月30日");
  EXPECT_EQ(DateTimeSeireki("あした", {2024, 2, 28, 0, 0}), "2024年2月29日");
  EXPECT_EQ(DateTimeSeireki("あさって", {2024, 2, 28, 0, 0}), "2024年3月1日");
  EXPECT_EQ(DateTimeSeireki("きのう", {2024, 3, 1, 0, 0}), "2024年2月29日");
  EXPECT_EQ(DateTimeSeireki("あした", {2023, 2, 28, 0, 0}), "2023年3月1日");
  EXPECT_EQ(DateTimeSeireki("あした", {2100, 2, 28, 0, 0}), "2100年3月1日");
  EXPECT_EQ(DateTimeSeireki("あした", {2000, 2, 28, 0, 0}), "2000年2月29日");
}

TEST(DateTimeRewriterTest, YearOffsetFromLeapDayLandsOnFebruary28) {
  // The year candidates show only the year; the shifted date stays valid.
  EXPECT_EQ(DateTimeSurfaces("らいねん", {2024, 2, 29, 12, 0}),
            (std::vector<std::string>{"2025年", "令和7年"}));
  EXPECT_EQ(DateTimeSurfaces("きょねん", {2024, 2, 29, 12, 0}),
            (std::vector<std::string>{"2023年", "令和5年"}));
}

TEST(DateTimeRewriterTest, WeekdaysCoverAWholeWeek) {
  const std::vector<std::string> expected = {"日曜日", "月曜日", "火曜日", "水曜日",
                                             "木曜日", "金曜日", "土曜日"};
  for (int i = 0; i < 7; ++i) {
    // 2026-11-15 is a Sunday.
    const auto candidates = ExpandDateTimeCandidates("きょう", {2026, 11, 15 + i, 0, 0});
    ASSERT_EQ(candidates.size(), 6u);
    EXPECT_EQ(candidates.back().surface, expected[static_cast<size_t>(i)]);
  }
}

TEST(DateTimeRewriterTest, WarekiSwitchesOnTheFirstDayOfEachEraAndWritesGannen) {
  EXPECT_EQ(DateTimeSurfaces("きょう", {2019, 4, 30, 0, 0})[1], "平成31年4月30日");
  EXPECT_EQ(DateTimeSurfaces("きょう", {2019, 5, 1, 0, 0})[1], "令和元年5月1日");
  EXPECT_EQ(DateTimeSurfaces("きょう", {1989, 1, 7, 0, 0})[1], "昭和64年1月7日");
  EXPECT_EQ(DateTimeSurfaces("きょう", {1989, 1, 8, 0, 0})[1], "平成元年1月8日");
  EXPECT_EQ(DateTimeSurfaces("きょう", {1926, 12, 25, 0, 0})[1], "昭和元年12月25日");
  EXPECT_EQ(DateTimeSurfaces("ことし", {2019, 5, 1, 0, 0}),
            (std::vector<std::string>{"2019年", "令和元年"}));
  EXPECT_EQ(DateTimeSurfaces("ことし", {1989, 1, 8, 0, 0}),
            (std::vector<std::string>{"1989年", "平成元年"}));
}

TEST(DateTimeRewriterTest, DropsOnlyTheWarekiCandidateBeforeShowa) {
  EXPECT_EQ(DateTimeSurfaces("きょう", {1926, 12, 24, 0, 0}),
            (std::vector<std::string>{"1926年12月24日", "1926/12/24", "1926-12-24",
                                      "12月24日（金）", "金曜日"}));
  EXPECT_EQ(DateTimeSurfaces("ことし", {1926, 12, 24, 0, 0}), (std::vector<std::string>{"1926年"}));
}

TEST(DateTimeRewriterTest, YearCandidatesUseTheEraOfTheShiftedDate) {
  // A year earlier than 2020-04-30 is 2019-04-30, still Heisei.
  EXPECT_EQ(DateTimeSurfaces("きょねん", {2020, 4, 30, 0, 0}),
            (std::vector<std::string>{"2019年", "平成31年"}));
  EXPECT_EQ(DateTimeSurfaces("きょねん", {2020, 5, 1, 0, 0}),
            (std::vector<std::string>{"2019年", "令和元年"}));
  EXPECT_EQ(DateTimeSurfaces("らいねん", {2018, 5, 1, 0, 0}),
            (std::vector<std::string>{"2019年", "令和元年"}));
  EXPECT_EQ(DateTimeSurfaces("らいねん", {1925, 12, 25, 0, 0}),
            (std::vector<std::string>{"1926年", "昭和元年"}));
  EXPECT_EQ(DateTimeSurfaces("らいねん", {1925, 12, 24, 0, 0}),
            (std::vector<std::string>{"1926年"}));
}

TEST(DateTimeRewriterTest, TimeFormatsPadOnlyTheMinuteAndHandleNoonAndMidnight) {
  EXPECT_EQ(DateTimeSurfaces("いま", {2026, 11, 17, 9, 5}),
            (std::vector<std::string>{"9:05", "9時5分", "午前9時5分"}));
  EXPECT_EQ(DateTimeSurfaces("いま", {2026, 11, 17, 0, 30}),
            (std::vector<std::string>{"0:30", "0時30分", "午前0時30分"}));
  EXPECT_EQ(DateTimeSurfaces("いま", {2026, 11, 17, 0, 0}),
            (std::vector<std::string>{"0:00", "0時", "午前0時"}));
  EXPECT_EQ(DateTimeSurfaces("いま", {2026, 11, 17, 12, 0}),
            (std::vector<std::string>{"12:00", "12時", "午後0時"}));
  EXPECT_EQ(DateTimeSurfaces("いま", {2026, 11, 17, 14, 0}),
            (std::vector<std::string>{"14:00", "14時", "午後2時"}));
  EXPECT_EQ(DateTimeSurfaces("いま", {2026, 11, 17, 23, 59}),
            (std::vector<std::string>{"23:59", "23時59分", "午後11時59分"}));

  const auto candidates = ExpandDateTimeCandidates("いま", kSpecExampleTime);
  ASSERT_EQ(candidates.size(), 3u);
  EXPECT_EQ(candidates[0].description, "時刻（24時間）");
  EXPECT_EQ(candidates[1].description, "時刻");
  EXPECT_EQ(candidates[2].description, "時刻（12時間）");
}

TEST(DateTimeRewriterTest, RejectsOutOfRangeClockValues) {
  for (const LocalDateTime now :
       {LocalDateTime{2026, 13, 1, 0, 0}, LocalDateTime{2026, 2, 29, 0, 0},
        LocalDateTime{2026, 4, 31, 0, 0}, LocalDateTime{2026, 1, 0, 0, 0},
        LocalDateTime{2026, 1, 1, 24, 0}, LocalDateTime{2026, 1, 1, 0, 60},
        LocalDateTime{2026, 1, 1, -1, 0}, LocalDateTime{0, 1, 1, 0, 0},
        LocalDateTime{10000, 1, 1, 0, 0}}) {
    EXPECT_TRUE(ExpandDateTimeCandidates("いま", now).empty());
    EXPECT_TRUE(ExpandDateTimeCandidates("きょう", now).empty());
  }
  // A shift past the supported years yields nothing rather than a 5-digit year.
  EXPECT_TRUE(ExpandDateTimeCandidates("あした", {9999, 12, 31, 0, 0}).empty());
  EXPECT_TRUE(ExpandDateTimeCandidates("らいねん", {9999, 1, 1, 0, 0}).empty());
}
