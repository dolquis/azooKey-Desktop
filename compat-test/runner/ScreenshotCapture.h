#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace azookey::compat_test {

bool CaptureRedactedDesktopPng(const std::filesystem::path& path,
                               const std::vector<RECT>& redaction_rects);

void ApplyPrivacyOverlayForTest(uint8_t* bgra, int width, int height, int stride, POINT origin,
                                const std::vector<RECT>& redaction_rects);

}  // namespace azookey::compat_test
