#include "BenchmarkCommandLine.h"

#include <stdexcept>
#include <utility>

#include "azookey/core/CommandLine.h"
#include "azookey/core/PlatformPaths.h"

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#endif

namespace azookey::bench {

std::filesystem::path Utf8Path(std::string_view value) { return core::Utf8Path(value); }

std::vector<std::string> Utf8CommandLineArguments(int argc, char** argv) {
#if defined(_WIN32)
  (void)argv;
  // The benchmarks keep a narrow `main`, so the wide argv has to be recovered
  // from the raw command line before core's UTF-8 conversion can run.
  int wide_argc = 0;
  wchar_t** wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
  if (!wide_argv || wide_argc != argc) {
    if (wide_argv) LocalFree(wide_argv);
    throw std::runtime_error("failed to read Unicode command-line arguments");
  }

  std::string error;
  auto arguments = core::Utf8CommandLineArguments(wide_argc, wide_argv, &error);
  LocalFree(wide_argv);
  if (!arguments) {
    throw std::runtime_error(error.empty() ? "failed to convert command-line argument to UTF-8"
                                           : error);
  }
  return std::move(*arguments);
#else
  std::string error;
  auto arguments = core::Utf8CommandLineArguments(argc, argv, &error);
  if (!arguments) {
    throw std::runtime_error(error.empty() ? "failed to read command-line arguments" : error);
  }
  return std::move(*arguments);
#endif
}

}  // namespace azookey::bench
