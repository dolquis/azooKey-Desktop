#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <string_view>

namespace azookey::compat_test {

bool IsRectWithinBounds(const RECT& rect, const RECT& bounds);
bool IsCandidateNearCaretAtDpi(const RECT& candidate, const RECT& caret, UINT dpi);
bool IsExpectedC002BackspaceTransition(std::wstring_view before, std::wstring_view after);

}  // namespace azookey::compat_test
