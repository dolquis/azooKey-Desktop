#include "azookey/core/Redaction.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace azookey::core {

namespace {

constexpr std::string_view kRedacted = "***redacted***";
constexpr std::string_view kUserProfile = "%USERPROFILE%";

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::string EnvironmentValue(const char* name) {
#ifdef _WIN32
  char* buffer = nullptr;
  size_t size = 0;
  if (_dupenv_s(&buffer, &size, name) != 0 || !buffer) return {};
  std::string value(buffer);
  std::free(buffer);
  return value;
#else
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
#endif
}

std::string ReplaceCaseInsensitive(std::string text, const std::string& needle,
                                   std::string_view replacement) {
  if (needle.empty()) return text;
  std::string lower_text = LowerAscii(text);
  const std::string lower_needle = LowerAscii(needle);
  const std::string lower_replacement = LowerAscii(std::string(replacement));
  size_t offset = 0;
  while ((offset = lower_text.find(lower_needle, offset)) != std::string::npos) {
    text.replace(offset, needle.size(), replacement);
    lower_text.replace(offset, needle.size(), lower_replacement);
    offset += replacement.size();
  }
  return text;
}

std::string EscapeJsonPath(std::string_view path) {
  std::string escaped;
  escaped.reserve(path.size() * 2);
  for (const char ch : path) {
    if (ch == '\\') escaped.push_back('\\');
    escaped.push_back(ch);
  }
  return escaped;
}

std::string NormalizeWindowsUserProfile(std::string text) {
  const std::array<std::string_view, 3> prefixes = {
      R"(:\users\)",
      R"(:\\users\\)",
      ":/users/",
  };
  for (const auto prefix : prefixes) {
    auto lower = LowerAscii(text);
    size_t start = 0;
    while ((start = lower.find(prefix, start)) != std::string::npos) {
      if (start == 0 || !std::isalpha(static_cast<unsigned char>(text[start - 1]))) {
        start += prefix.size();
        continue;
      }
      const size_t replacement_start = start - 1;
      const size_t user_start = start + prefix.size();
      size_t user_end = user_start;
      while (user_end < text.size() && text[user_end] != '\\' && text[user_end] != '/' &&
             text[user_end] != '"' && !std::isspace(static_cast<unsigned char>(text[user_end]))) {
        ++user_end;
      }
      if (user_end == user_start) {
        start = user_start;
        continue;
      }
      text.replace(replacement_start, user_end - replacement_start, kUserProfile);
      lower = LowerAscii(text);
      start = replacement_start + kUserProfile.size();
    }
  }
  return text;
}

std::string RedactTokenPrefix(std::string text, std::string_view prefix) {
  const auto lower_prefix = LowerAscii(std::string(prefix));
  auto lower = LowerAscii(text);
  size_t start = 0;
  while ((start = lower.find(lower_prefix, start)) != std::string::npos) {
    size_t end = start + prefix.size();
    while (end < text.size()) {
      const unsigned char ch = static_cast<unsigned char>(text[end]);
      if (std::isspace(ch) || ch == '"' || ch == '\'' || ch == ',' || ch == ';') break;
      ++end;
    }
    text.replace(start, end - start, kRedacted);
    lower = LowerAscii(text);
    start += kRedacted.size();
  }
  return text;
}

}  // namespace

std::string RedactFreeText(std::string text) {
  const std::array<std::pair<const char*, const char*>, 2> paths = {
      std::pair{"LOCALAPPDATA", "%LOCALAPPDATA%"},
      std::pair{"USERPROFILE", "%USERPROFILE%"},
  };
  for (const auto& [environment, replacement] : paths) {
    const auto value = EnvironmentValue(environment);
    if (!value.empty()) {
      text = ReplaceCaseInsensitive(std::move(text), value, replacement);
      text = ReplaceCaseInsensitive(std::move(text), EscapeJsonPath(value), replacement);
    }
  }
  text = NormalizeWindowsUserProfile(std::move(text));
  text = RedactTokenPrefix(std::move(text), "sk-");
  text = RedactTokenPrefix(std::move(text), "dpapi:");
  text = RedactTokenPrefix(std::move(text), "Bearer ");
  return text;
}

}  // namespace azookey::core
