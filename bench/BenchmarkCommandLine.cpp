#include "BenchmarkCommandLine.h"

#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#endif

namespace azookey::bench {

std::filesystem::path Utf8Path(std::string_view value) {
  std::u8string utf8;
  utf8.reserve(value.size());
  for (const char ch : value) {
    utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(ch)));
  }
  return std::filesystem::path(utf8);
}

std::vector<std::string> Utf8CommandLineArguments(int argc, char** argv) {
#if defined(_WIN32)
  (void)argv;
  int wide_argc = 0;
  wchar_t** wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
  if (!wide_argv || wide_argc != argc) {
    if (wide_argv) LocalFree(wide_argv);
    throw std::runtime_error("failed to read Unicode command-line arguments");
  }

  std::vector<std::string> arguments;
  arguments.reserve(static_cast<size_t>(wide_argc));
  for (int i = 0; i < wide_argc; ++i) {
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1,
                                             nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
      LocalFree(wide_argv);
      throw std::runtime_error("failed to convert command-line argument to UTF-8");
    }
    std::string converted(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, converted.data(),
                            required, nullptr, nullptr) != required) {
      LocalFree(wide_argv);
      throw std::runtime_error("failed to convert command-line argument to UTF-8");
    }
    converted.pop_back();
    arguments.push_back(std::move(converted));
  }
  LocalFree(wide_argv);
  return arguments;
#else
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    if (!argv[i]) {
      throw std::runtime_error("failed to read command-line arguments");
    }
    arguments.emplace_back(argv[i]);
  }
  return arguments;
#endif
}

}  // namespace azookey::bench
