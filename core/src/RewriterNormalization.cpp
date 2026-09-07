#include <limits>

#include "azookey/core/RewriterIndex.h"

#ifdef _WIN32
#include <icu.h>
#else
#include <unicode/unorm2.h>
#include <unicode/ustring.h>
#endif

#include "azookey/core/Utf8.h"

namespace azookey::core {

std::string NormalizeRewriterReading(std::string_view input) {
  if (input.empty() || input.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
    return {};
  // UTF-16 needs no more units than the original UTF-8 byte count.
  std::vector<UChar> utf16(input.size());
  int32_t utf16_size = 0;
  UErrorCode error = U_ZERO_ERROR;
  u_strFromUTF8(utf16.data(), static_cast<int32_t>(utf16.size()), &utf16_size, input.data(),
                static_cast<int32_t>(input.size()), &error);
  if (U_FAILURE(error)) return {};
  error = U_ZERO_ERROR;
  const auto* normalizer = unorm2_getNFKCInstance(&error);
  if (U_FAILURE(error)) return {};
  const auto required = unorm2_normalize(normalizer, utf16.data(), utf16_size, nullptr, 0, &error);
  if (error != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(error)) return {};
  if (required <= 0) return {};
  error = U_ZERO_ERROR;
  std::vector<UChar> normalized(static_cast<size_t>(required));
  const auto length =
      unorm2_normalize(normalizer, utf16.data(), utf16_size, normalized.data(), required, &error);
  if (U_FAILURE(error)) return {};
  std::string result;
  for (int32_t i = 0; i < length; ++i) {
    char32_t cp = normalized[static_cast<size_t>(i)];
    if (cp >= 0xd800 && cp <= 0xdbff) {
      if (++i >= length) return {};
      const char32_t low = normalized[static_cast<size_t>(i)];
      if (low < 0xdc00 || low > 0xdfff) return {};
      cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
    }
    // Unicode White_Space plus the four ASCII separators removed by Python str.isspace().
    if ((cp >= 0x09 && cp <= 0x0d) || (cp >= 0x1c && cp <= 0x20) || cp == 0x85 || cp == 0xa0 ||
        cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
        cp == 0x202f || cp == 0x205f || cp == 0x3000)
      continue;
    if (cp >= 0x30a1 && cp <= 0x30f6) cp -= 0x60;
    AppendUtf8(result, cp);
  }
  return result;
}

std::string NormalizeEmojiTrigger(std::string_view input) {
  std::string result;
  for (char ch : input) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '+' || ch == '-')
      result.push_back(ch);
  }
  return result;
}

}  // namespace azookey::core
