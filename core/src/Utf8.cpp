#include "azookey/core/Utf8.h"

namespace azookey::core {

bool DecodeNextUtf8(std::string_view input, size_t& offset, char32_t& codepoint) {
  const size_t start = offset;
  if (start >= input.size()) return false;
  const auto first = static_cast<unsigned char>(input[start]);
  codepoint = first;
  offset = start + 1;
  if (first < 0x80) return true;

  size_t width = 0;
  char32_t value = 0;
  if ((first & 0xe0) == 0xc0) {
    width = 2;
    value = first & 0x1f;
  } else if ((first & 0xf0) == 0xe0) {
    width = 3;
    value = first & 0x0f;
  } else if ((first & 0xf8) == 0xf0) {
    width = 4;
    value = first & 0x07;
  } else {
    return false;
  }
  if (width > input.size() - start) return false;
  for (size_t index = 1; index < width; ++index) {
    const auto byte = static_cast<unsigned char>(input[start + index]);
    if ((byte & 0xc0) != 0x80) return false;
    value = (value << 6) | (byte & 0x3f);
  }
  if ((width == 2 && value < 0x80) || (width == 3 && value < 0x800) ||
      (width == 4 && value < 0x10000) || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
    return false;
  }
  offset = start + width;
  codepoint = value;
  return true;
}

std::string TakeLastUtf8Codepoints(std::string_view input, size_t max_codepoints) {
  size_t start = input.size();
  for (size_t count = 0; start > 0 && count < max_codepoints; ++count) {
    --start;
    while (start > 0 && (static_cast<unsigned char>(input[start]) & 0xc0) == 0x80) {
      --start;
    }
  }
  return std::string(input.substr(start));
}

}  // namespace azookey::core
