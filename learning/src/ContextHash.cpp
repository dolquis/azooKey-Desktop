#include "azookey/learning/ContextHash.h"

#include <array>
#include <cstdint>
#include <cstdio>

#include "azookey/learning/Sha256.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace azookey::learning {

namespace {
bool IsContinuationByte(char ch) { return (static_cast<unsigned char>(ch) & 0xC0) == 0x80; }

// Suffix holding the last `count` code points. A truncated sequence counts as
// one code point per stray byte, which keeps the walk bounded.
std::string_view LastCodePoints(std::string_view text, size_t count) {
  size_t begin = text.size();
  for (size_t taken = 0; taken < count && begin > 0; ++taken) {
    size_t lead = begin - 1;
    size_t steps = 0;
    while (lead > 0 && IsContinuationByte(text[lead]) && steps < 3) {
      --lead;
      ++steps;
    }
    begin = lead;
  }
  return text.substr(begin);
}

#ifdef _WIN32
std::wstring Utf8ToWide(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                       static_cast<int>(utf8.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring wide(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                      wide.data(), size);
  return wide;
}

std::string WideToUtf8(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string utf8(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size,
                      nullptr, nullptr);
  return utf8;
}
#endif
}  // namespace

std::string NormalizeNfc(std::string_view utf8) {
#ifdef _WIN32
  const auto wide = Utf8ToWide(utf8);
  if (wide.empty()) return std::string(utf8);
  const int input_size = static_cast<int>(wide.size());
  int estimate = NormalizeString(NormalizationC, wide.data(), input_size, nullptr, 0);
  // NormalizeString may ask for a larger buffer on the second call; retry a
  // few times as its documentation describes.
  for (int attempt = 0; attempt < 4 && estimate > 0; ++attempt) {
    std::wstring normalized(static_cast<size_t>(estimate), L'\0');
    const int written =
        NormalizeString(NormalizationC, wide.data(), input_size, normalized.data(), estimate);
    if (written > 0) {
      normalized.resize(static_cast<size_t>(written));
      return WideToUtf8(normalized);
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
    estimate = -written;
  }
  return std::string(utf8);
#else
  return std::string(utf8);
#endif
}

std::string ContextHash(std::string_view left_context_utf8) {
  if (left_context_utf8.empty()) return std::string(kEmptyContextHash);
  const auto normalized = NormalizeNfc(left_context_utf8);
  const auto digest = Sha256(LastCodePoints(normalized, kContextHashCodePoints));
  const uint32_t prefix =
      (static_cast<uint32_t>(digest[0]) << 24) | (static_cast<uint32_t>(digest[1]) << 16) |
      (static_cast<uint32_t>(digest[2]) << 8) | static_cast<uint32_t>(digest[3]);
  std::array<char, 11> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "0x%08x", static_cast<unsigned>(prefix));
  return std::string(buffer.data());
}

}  // namespace azookey::learning
