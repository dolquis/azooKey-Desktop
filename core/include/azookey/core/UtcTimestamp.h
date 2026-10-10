#pragma once

#include <charconv>
#include <chrono>
#include <cstddef>
#include <string_view>

namespace azookey::core {

// UTC RFC 3339 with second precision, including calendar and clock validation.
inline bool IsUtcSecondTimestamp(std::string_view text) noexcept {
  if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
      text[16] != ':' || text[19] != 'Z') {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16 || i == 19) continue;
    if (text[i] < '0' || text[i] > '9') return false;
  }
  const auto number = [&](std::size_t offset, std::size_t count) {
    int result{};
    std::from_chars(text.data() + offset, text.data() + offset + count, result);
    return result;
  };
  const int year = number(0, 4);
  const auto date = std::chrono::year_month_day(
      std::chrono::year(year), std::chrono::month(number(5, 2)), std::chrono::day(number(8, 2)));
  return year != 0 && date.ok() && number(11, 2) <= 23 && number(14, 2) <= 59 &&
         number(17, 2) <= 59;
}

}  // namespace azookey::core
