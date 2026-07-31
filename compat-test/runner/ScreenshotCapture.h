#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace azookey::compat_test {

bool WriteGeometryOverlayPng(const std::filesystem::path& path,
                             const std::vector<RECT>& highlight_rects);

void RenderGeometryOverlay(uint8_t* bgra, int width, int height, int stride, POINT origin,
                           const std::vector<RECT>& highlight_rects);

}  // namespace azookey::compat_test
