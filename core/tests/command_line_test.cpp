#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "azookey/core/CommandLine.h"
#include "azookey/core/PlatformPaths.h"

namespace {

// Written as escapes so the expectations pin actual UTF-8 bytes rather than
// whatever the compiler makes of a literal in this file.
constexpr const char* kJapanesePath =
    "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e/\xe8\xbe\x9e\xe6\x9b\xb8.json";
constexpr const char* kJapaneseQuery = "\xe3\x82\x88\xe3\x81\xbf";

}  // namespace

TEST(Utf8PathTest, KeepsUtf8BytesAcrossThePathBoundary) {
  const auto path = azookey::core::Utf8Path(kJapanesePath);
  EXPECT_EQ(azookey::core::PathToUtf8(path), kJapanesePath);
  EXPECT_EQ(azookey::core::PathToUtf8(path.filename()), "\xe8\xbe\x9e\xe6\x9b\xb8.json");
}

TEST(Utf8PathTest, LeavesAsciiAndEmptyInputUnchanged) {
  EXPECT_EQ(azookey::core::PathToUtf8(azookey::core::Utf8Path("plain/user_dict.json")),
            "plain/user_dict.json");
  EXPECT_TRUE(azookey::core::Utf8Path("").empty());
}

#if defined(_WIN32)

TEST(Utf8CommandLineArgumentsTest, ConvertsWideArgumentsToUtf8) {
  const wchar_t* argv[] = {
      L"azookey_inference_host.exe", L"lookup", L"--mode", L"exact", L"--query", L"\u3088\u307f"};
  std::string error;
  auto arguments = azookey::core::Utf8CommandLineArguments(6, argv, &error);
  ASSERT_TRUE(arguments.has_value()) << error;
  ASSERT_EQ(arguments->size(), 6u);
  EXPECT_EQ((*arguments)[1], "lookup");
  EXPECT_EQ((*arguments)[5], kJapaneseQuery);
}

TEST(Utf8CommandLineArgumentsTest, ConvertsNonAsciiPathArgument) {
  const std::wstring wide_path = L"\u65e5\u672c\u8a9e/\u8f9e\u66f8.json";
  const wchar_t* argv[] = {L"azookey_inference_host.exe", L"--user-dict", wide_path.c_str()};
  auto arguments = azookey::core::Utf8CommandLineArguments(3, argv);
  ASSERT_TRUE(arguments.has_value());
  EXPECT_EQ((*arguments)[2], kJapanesePath);
  EXPECT_EQ(azookey::core::Utf8Path((*arguments)[2]).wstring(), wide_path);
}

TEST(Utf8CommandLineArgumentsTest, RejectsUnpairedSurrogate) {
  // A lone high surrogate has no UTF-8 encoding: fail instead of substituting
  // U+FFFD, which would silently point at a different path.
  const wchar_t lone_surrogate[] = {L'a', static_cast<wchar_t>(0xD800), L'\0'};
  const wchar_t* argv[] = {L"azookey_inference_host.exe", lone_surrogate};
  std::string error;
  EXPECT_FALSE(azookey::core::Utf8CommandLineArguments(2, argv, &error).has_value());
  EXPECT_EQ(error, "failed to convert command-line argument to UTF-8");
}

TEST(Utf8CommandLineArgumentsTest, AcceptsEmptyArgument) {
  const wchar_t* argv[] = {L"azookey_inference_host.exe", L""};
  auto arguments = azookey::core::Utf8CommandLineArguments(2, argv);
  ASSERT_TRUE(arguments.has_value());
  EXPECT_TRUE((*arguments)[1].empty());
}

#else

TEST(Utf8CommandLineArgumentsTest, CopiesNarrowArgumentBytes) {
  const char* argv[] = {"azookey_inference_host", "--query", kJapaneseQuery};
  std::string error;
  auto arguments = azookey::core::Utf8CommandLineArguments(3, argv, &error);
  ASSERT_TRUE(arguments.has_value()) << error;
  ASSERT_EQ(arguments->size(), 3u);
  EXPECT_EQ((*arguments)[2], kJapaneseQuery);
}

TEST(Utf8CommandLineArgumentsTest, RejectsNullArgument) {
  const char* argv[] = {"azookey_inference_host", nullptr};
  std::string error;
  EXPECT_FALSE(azookey::core::Utf8CommandLineArguments(2, argv, &error).has_value());
  EXPECT_EQ(error, "failed to read command-line arguments");
}

#endif
