#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <string_view>

namespace azookey::compat_test {

bool IsRectWithinBounds(const RECT& rect, const RECT& bounds);
bool IsCandidateNearCaretAtDpi(const RECT& candidate, const RECT& caret, UINT dpi);

struct C002BackspaceScenario {
  static constexpr std::string_view kInput = "nihongo";
  static constexpr std::wstring_view kBefore = L"にほんご";
  static constexpr std::array<std::wstring_view, 2> kExpectedAfter = {L"にほん", L"にほんg"};
};

bool IsExpectedC002BackspaceTransition(std::wstring_view before, std::wstring_view after);

}  // namespace azookey::compat_test
