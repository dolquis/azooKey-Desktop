#include "azookey/core/DateTimeRewriter.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace azookey::core {
namespace date_time_rewriter_detail {

// docs/candidate-rewriter-spec.md §20.3. Add a row to add a reading.
enum class Kind { Day, Year, Time };

struct ReadingRow {
  std::string_view reading;
  Kind kind;
  int offset;
};

// clang-format off
constexpr ReadingRow kReadings[] = {
    {"きょう", Kind::Day, 0},
    {"ほんじつ", Kind::Day, 0},
    {"あした", Kind::Day, 1},
    {"あす", Kind::Day, 1},
    {"みょうにち", Kind::Day, 1},
    {"あさって", Kind::Day, 2},
    {"みょうごにち", Kind::Day, 2},
    {"きのう", Kind::Day, -1},
    {"さくじつ", Kind::Day, -1},
    {"おととい", Kind::Day, -2},
    {"いっさくじつ", Kind::Day, -2},
    {"ことし", Kind::Year, 0},
    {"らいねん", Kind::Year, 1},
    {"きょねん", Kind::Year, -1},
    {"さくねん", Kind::Year, -1},
    {"いま", Kind::Time, 0},
};
// clang-format on

struct CivilDate {
  int year;
  int month;
  int day;
};

// §20.5. Newest first; a date on or after `start` belongs to that era.
struct Era {
  std::string_view name;
  CivilDate start;
};

constexpr Era kEras[] = {
    {"令和", {2019, 5, 1}},
    {"平成", {1989, 1, 8}},
    {"昭和", {1926, 12, 25}},
};

constexpr int kMinYear = 1;
constexpr int kMaxYear = 9999;

bool IsLeapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int DaysInMonth(int year, int month) {
  static constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && IsLeapYear(year) ? 29 : kDays[static_cast<size_t>(month - 1)];
}

bool IsValidDate(const CivilDate& date) {
  return date.year >= kMinYear && date.year <= kMaxYear && date.month >= 1 && date.month <= 12 &&
         date.day >= 1 && date.day <= DaysInMonth(date.year, date.month);
}

// Days since 1970-01-01 in the proleptic Gregorian calendar (H. Hinnant's
// days_from_civil), so day offsets cross month ends, year ends and leap days.
int64_t DaysFromCivil(const CivilDate& date) {
  const int64_t y = date.month <= 2 ? date.year - 1 : date.year;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t mp = (date.month + 9) % 12;
  const int64_t doy = (153 * mp + 2) / 5 + date.day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

CivilDate CivilFromDays(int64_t days) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t day = doy - (153 * mp + 2) / 5 + 1;
  const int64_t month = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
  return {static_cast<int>(year), static_cast<int>(month), static_cast<int>(day)};
}

// 0 = Sunday. 1970-01-01 was a Thursday.
int Weekday(const CivilDate& date) {
  const int64_t days = DaysFromCivil(date);
  return static_cast<int>(((days % 7) + 7 + 4) % 7);
}

bool OnOrAfter(const CivilDate& date, const CivilDate& start) {
  if (date.year != start.year) return date.year > start.year;
  if (date.month != start.month) return date.month > start.month;
  return date.day >= start.day;
}

// `令和8年` / `令和元年`, or nothing before the oldest era in the table.
std::optional<std::string> WarekiYear(const CivilDate& date) {
  for (const auto& era : kEras) {
    if (!OnOrAfter(date, era.start)) continue;
    const int year = date.year - era.start.year + 1;
    std::string text(era.name);
    text += year == 1 ? "元" : std::to_string(year);
    text += "年";
    return text;
  }
  return std::nullopt;
}

std::string TwoDigits(int value) {
  return value < 10 ? "0" + std::to_string(value) : std::to_string(value);
}

std::string FourDigits(int value) {
  std::string text = std::to_string(value);
  while (text.size() < 4) text.insert(text.begin(), '0');
  return text;
}

constexpr std::array<std::string_view, 7> kWeekdays{"日", "月", "火", "水", "木", "金", "土"};

Candidate MakeCandidate(const std::string& reading, std::string surface, const char* description,
                        const char* tag) {
  Candidate candidate;
  candidate.surface = std::move(surface);
  candidate.reading = reading;
  candidate.source = CandidateSource::Heuristic;
  candidate.debug_info = std::string("date-rewriter:") + tag;
  candidate.description = description;
  return candidate;
}

std::vector<Candidate> DayCandidates(const std::string& reading, const CivilDate& date) {
  const std::string month_day = std::to_string(date.month) + "月" + std::to_string(date.day) + "日";
  const std::string_view weekday = kWeekdays[static_cast<size_t>(Weekday(date))];
  std::vector<Candidate> result;
  result.push_back(
      MakeCandidate(reading, FourDigits(date.year) + "年" + month_day, "西暦", "seireki"));
  if (const auto wareki = WarekiYear(date))
    result.push_back(MakeCandidate(reading, *wareki + month_day, "和暦", "wareki"));
  result.push_back(MakeCandidate(
      reading, FourDigits(date.year) + "/" + TwoDigits(date.month) + "/" + TwoDigits(date.day),
      "西暦（スラッシュ）", "slash"));
  result.push_back(MakeCandidate(
      reading, FourDigits(date.year) + "-" + TwoDigits(date.month) + "-" + TwoDigits(date.day),
      "ISO 8601", "iso8601"));
  result.push_back(MakeCandidate(reading, month_day + "（" + std::string(weekday) + "）",
                                 "月日と曜日", "month-day-weekday"));
  result.push_back(MakeCandidate(reading, std::string(weekday) + "曜日", "曜日", "weekday"));
  return result;
}

std::vector<Candidate> YearCandidates(const std::string& reading, const CivilDate& date) {
  std::vector<Candidate> result;
  result.push_back(MakeCandidate(reading, FourDigits(date.year) + "年", "西暦", "year-seireki"));
  if (const auto wareki = WarekiYear(date))
    result.push_back(MakeCandidate(reading, *wareki, "和暦", "year-wareki"));
  return result;
}

std::string HourMinute(int hour, int minute) {
  std::string text = std::to_string(hour) + "時";
  if (minute != 0) text += std::to_string(minute) + "分";
  return text;
}

std::vector<Candidate> TimeCandidates(const std::string& reading, int hour, int minute) {
  std::vector<Candidate> result;
  result.push_back(MakeCandidate(reading, std::to_string(hour) + ":" + TwoDigits(minute),
                                 "時刻（24時間）", "time-24h"));
  result.push_back(MakeCandidate(reading, HourMinute(hour, minute), "時刻", "time"));
  result.push_back(MakeCandidate(
      reading, std::string(hour < 12 ? "午前" : "午後") + HourMinute(hour % 12, minute),
      "時刻（12時間）", "time-12h"));
  return result;
}

}  // namespace date_time_rewriter_detail

std::vector<Candidate> ExpandDateTimeCandidates(const std::string& reading,
                                                const LocalDateTime& now) {
  using namespace date_time_rewriter_detail;
  const ReadingRow* row = nullptr;
  for (const auto& candidate : kReadings) {
    if (candidate.reading == reading) {
      row = &candidate;
      break;
    }
  }
  if (row == nullptr) return {};

  const CivilDate today{now.year, now.month, now.day};
  if (!IsValidDate(today) || now.hour < 0 || now.hour > 23 || now.minute < 0 || now.minute > 59)
    return {};

  switch (row->kind) {
    case Kind::Day: {
      const CivilDate date = CivilFromDays(DaysFromCivil(today) + row->offset);
      if (!IsValidDate(date)) return {};
      return DayCandidates(reading, date);
    }
    case Kind::Year: {
      CivilDate date{today.year + row->offset, today.month, today.day};
      if (date.year < kMinYear || date.year > kMaxYear) return {};
      // §20.3: Feb 29 shifted into a common year becomes Feb 28.
      if (date.month == 2 && date.day == 29 && !IsLeapYear(date.year)) date.day = 28;
      return YearCandidates(reading, date);
    }
    case Kind::Time:
      return TimeCandidates(reading, now.hour, now.minute);
  }
  return {};
}

}  // namespace azookey::core
