#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace azookey::core {

// Candidate pages (docs/legacy-parity-spec.md §1.4「候補のページ」). The list is
// split into pages of nine from the first candidate, unless an app that draws
// the list split it with SetPageIndex (docs/tsf-deep-integration-spec.md §2.3).
// The page is always derived from the split and the selected index: the core,
// the candidate window and the UI element share these functions and keep no
// page of their own. Only CandidatePageStarts allocates.
inline constexpr size_t kCandidatePageSize = 9;

// An app's split is usable when it starts at 0, strictly increases and stays
// below the candidate count. Every function below reads an unusable or empty
// split as nine per page.
inline bool IsValidCandidatePageStarts(const std::vector<uint32_t>& starts, size_t count) {
  if (starts.empty() || starts.front() != 0) return false;
  for (size_t i = 0; i < starts.size(); ++i) {
    if (starts[i] >= count) return false;
    if (i > 0 && starts[i] <= starts[i - 1]) return false;
  }
  return true;
}

inline size_t CandidatePageCount(size_t count, const std::vector<uint32_t>& starts = {}) {
  if (IsValidCandidatePageStarts(starts, count)) return starts.size();
  return (count + kCandidatePageSize - 1) / kCandidatePageSize;
}

// The first index of every page.
inline std::vector<uint32_t> CandidatePageStarts(size_t count,
                                                 const std::vector<uint32_t>& starts = {}) {
  if (IsValidCandidatePageStarts(starts, count)) return starts;
  std::vector<uint32_t> pages;
  for (size_t start = 0; start < count; start += kCandidatePageSize)
    pages.push_back(static_cast<uint32_t>(start));
  return pages;
}

// [begin, end) of page `page` (counted from 0), and its number.
struct CandidatePageRange {
  size_t page{0};
  size_t begin{0};
  size_t end{0};
};

inline CandidatePageRange CandidatePageNumbered(size_t page, size_t count,
                                                const std::vector<uint32_t>& starts = {}) {
  if (IsValidCandidatePageStarts(starts, count)) {
    page = std::min(page, starts.size() - 1);
    return {page, starts[page], page + 1 < starts.size() ? starts[page + 1] : count};
  }
  if (count == 0) return {};
  page = std::min(page, (count - 1) / kCandidatePageSize);
  const size_t begin = page * kCandidatePageSize;
  return {page, begin, std::min(begin + kCandidatePageSize, count)};
}

// The page holding `index`, clamped into the list. All zero for an empty list.
inline CandidatePageRange CandidatePageAt(size_t index, size_t count,
                                          const std::vector<uint32_t>& starts = {}) {
  if (count == 0) return {};
  index = std::min(index, count - 1);
  if (!IsValidCandidatePageStarts(starts, count))
    return CandidatePageNumbered(index / kCandidatePageSize, count);
  // The last page starting at or before the index; starts[0] is 0.
  const auto after = std::upper_bound(starts.begin(), starts.end(), index);
  return CandidatePageNumbered(static_cast<size_t>(after - starts.begin()) - 1, count, starts);
}

// The candidate a digit 1-9 picks on the page holding `selected`; nullopt when
// that page has no n-th candidate.
inline std::optional<size_t> CandidateByDigit(size_t selected, int digit, size_t count,
                                              const std::vector<uint32_t>& starts = {}) {
  if (count == 0 || digit < 1 || digit > static_cast<int>(kCandidatePageSize)) return std::nullopt;
  const CandidatePageRange range = CandidatePageAt(selected, count, starts);
  const size_t target = range.begin + static_cast<size_t>(digit - 1);
  if (target >= range.end) return std::nullopt;
  return target;
}

// The candidate PageDown (forward) or PageUp moves `selected` to: the same
// position on the next or previous page, or that page's last candidate when it
// is shorter. The last page wraps to the first and back. nullopt when the list
// has one page.
inline std::optional<size_t> CandidateOnAdjacentPage(size_t selected, bool forward, size_t count,
                                                     const std::vector<uint32_t>& starts = {}) {
  const size_t pages = CandidatePageCount(count, starts);
  if (pages <= 1) return std::nullopt;
  const CandidatePageRange current = CandidatePageAt(selected, count, starts);
  const size_t offset = std::min(selected, count - 1) - current.begin;
  const size_t page = forward ? (current.page + 1) % pages : (current.page + pages - 1) % pages;
  const CandidatePageRange target = CandidatePageNumbered(page, count, starts);
  return std::min(target.begin + offset, target.end - 1);
}

}  // namespace azookey::core
