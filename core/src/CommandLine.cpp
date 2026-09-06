#include "azookey/core/CommandLine.h"

#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace azookey::core {
namespace {

bool SetError(std::string* error, const char* message) {
  if (error) *error = message;
  return false;
}

}  // namespace

#if defined(_WIN32)

std::optional<std::string> Utf8FromWideArgument(const wchar_t* value, std::string* error) {
  if (!value) {
    SetError(error, "failed to read command-line arguments");
    return std::nullopt;
  }
  if (*value == L'\0') return std::string();

  // WC_ERR_INVALID_CHARS makes an unpaired surrogate fail instead of turning
  // into U+FFFD, so a malformed argument is rejected rather than mistaken for a
  // valid but different path or query.
  const int required =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    SetError(error, "failed to convert command-line argument to UTF-8");
    return std::nullopt;
  }
  std::string converted(static_cast<size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, converted.data(), required,
                          nullptr, nullptr) != required) {
    SetError(error, "failed to convert command-line argument to UTF-8");
    return std::nullopt;
  }
  // -1 as the input length makes the terminator part of the written length.
  converted.pop_back();
  return converted;
}

std::optional<std::vector<std::string>> Utf8CommandLineArguments(int argc,
                                                                 const wchar_t* const* argv,
                                                                 std::string* error) {
  if (argc < 0 || (argc > 0 && !argv)) {
    SetError(error, "failed to read command-line arguments");
    return std::nullopt;
  }
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    auto converted = Utf8FromWideArgument(argv[i], error);
    if (!converted) return std::nullopt;
    arguments.push_back(std::move(*converted));
  }
  return arguments;
}

#else

std::optional<std::vector<std::string>> Utf8CommandLineArguments(int argc, const char* const* argv,
                                                                 std::string* error) {
  if (argc < 0 || (argc > 0 && !argv)) {
    SetError(error, "failed to read command-line arguments");
    return std::nullopt;
  }
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    if (!argv[i]) {
      SetError(error, "failed to read command-line arguments");
      return std::nullopt;
    }
    arguments.emplace_back(argv[i]);
  }
  return arguments;
}

#endif

}  // namespace azookey::core
