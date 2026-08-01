#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace azookey::compat_test {

bool IsRectWithinBounds(const RECT& rect, const RECT& bounds);
bool IsCandidateNearCaretAtDpi(const RECT& candidate, const RECT& caret, UINT dpi);

}  // namespace azookey::compat_test
