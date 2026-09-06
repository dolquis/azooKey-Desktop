#pragma once

#include <optional>
#include <string>
#include <vector>

namespace azookey::core {

#if defined(_WIN32)

// Convert one UTF-16 command-line argument to UTF-8. Unpaired surrogates are
// reported as an error instead of being replaced, so a caller never silently
// substitutes a different string for a path or a query.
std::optional<std::string> Utf8FromWideArgument(const wchar_t* value, std::string* error = nullptr);

// Convert the arguments of a Windows `wmain` entry point (argv[0] included) to
// UTF-8. Wide argv is the only lossless argv on Windows: the narrow `main`
// argv is encoded in the process active code page, which drops or replaces
// characters outside it.
std::optional<std::vector<std::string>> Utf8CommandLineArguments(int argc,
                                                                 const wchar_t* const* argv,
                                                                 std::string* error = nullptr);

#else

// POSIX argv already carries the bytes the caller passed, so this only copies
// them and keeps the call site identical across platforms.
std::optional<std::vector<std::string>> Utf8CommandLineArguments(int argc, const char* const* argv,
                                                                 std::string* error = nullptr);

#endif

}  // namespace azookey::core
