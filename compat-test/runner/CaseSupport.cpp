#include "runner/CaseSupport.h"

#include <cstdlib>

namespace azookey::compat_test {

bool IsRectWithinBounds(const RECT& rect, const RECT& bounds) {
  return rect.left >= bounds.left && rect.top >= bounds.top && rect.right <= bounds.right &&
         rect.bottom <= bounds.bottom && rect.right > rect.left && rect.bottom > rect.top;
}

bool IsCandidateNearCaretAtDpi(const RECT& candidate, const RECT& caret, UINT dpi) {
  if (dpi == 0) return false;
  const LONG horizontal_tolerance = MulDiv(64, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
  const LONG maximum_vertical_gap = MulDiv(100, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
  const LONG top_tolerance = MulDiv(4, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
  return std::abs(candidate.left - caret.left) <= horizontal_tolerance &&
         candidate.top >= caret.bottom - top_tolerance &&
         candidate.top - caret.bottom <= maximum_vertical_gap;
}

bool IsExpectedC002BackspaceTransition(std::wstring_view before, std::wstring_view after) {
  return before == L"にほんご" && after == L"にほんg";
}

}  // namespace azookey::compat_test
