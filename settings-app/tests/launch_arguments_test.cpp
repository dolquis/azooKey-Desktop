#include <gtest/gtest.h>

#include "LaunchArguments.h"

namespace {

using azookey::settings::LaunchArgumentStatus;

constexpr GUID kProfile = {
    0x8d80b94e, 0x75ba, 0x47d4, {0x90, 0xb9, 0x68, 0x97, 0x7a, 0x5a, 0x6e, 0x2b}};

TEST(SettingsLaunchArgumentsTest, ParsesLauncherRoundTrip) {
  const auto arguments = azookey::settings::ParseLaunchArguments(
      L"--langid 0x0411 --profile {8D80B94E-75BA-47D4-90B9-68977A5A6E2B}");

  EXPECT_FALSE(arguments.parse_error);
  EXPECT_EQ(arguments.langid_status, LaunchArgumentStatus::Valid);
  EXPECT_EQ(arguments.langid, 0x0411);
  EXPECT_EQ(arguments.profile_status, LaunchArgumentStatus::Valid);
  EXPECT_TRUE(IsEqualGUID(arguments.profile, kProfile));
}

TEST(SettingsLaunchArgumentsTest, KeepsFollowingProfileWhenLangIdValueIsMissing) {
  const auto arguments = azookey::settings::ParseLaunchArguments(
      L"--langid --profile {8D80B94E-75BA-47D4-90B9-68977A5A6E2B}");

  EXPECT_EQ(arguments.langid_status, LaunchArgumentStatus::Invalid);
  EXPECT_EQ(arguments.profile_status, LaunchArgumentStatus::Valid);
  EXPECT_TRUE(IsEqualGUID(arguments.profile, kProfile));
}

TEST(SettingsLaunchArgumentsTest, RejectsTrailingOptionWithoutValue) {
  const auto arguments = azookey::settings::ParseLaunchArguments(L"--langid");

  EXPECT_EQ(arguments.langid_status, LaunchArgumentStatus::Invalid);
  EXPECT_EQ(arguments.profile_status, LaunchArgumentStatus::NotSupplied);
}

TEST(SettingsLaunchArgumentsTest, RejectsInvalidLangIdAndProfile) {
  const auto arguments =
      azookey::settings::ParseLaunchArguments(L"--langid 0411 --profile not-a-guid");

  EXPECT_EQ(arguments.langid_status, LaunchArgumentStatus::Invalid);
  EXPECT_EQ(arguments.profile_status, LaunchArgumentStatus::Invalid);
}

TEST(SettingsLaunchArgumentsTest, DistinguishesArgumentsThatWereNotSupplied) {
  const auto arguments = azookey::settings::ParseLaunchArguments(L"");

  EXPECT_FALSE(arguments.parse_error);
  EXPECT_EQ(arguments.langid_status, LaunchArgumentStatus::NotSupplied);
  EXPECT_EQ(arguments.profile_status, LaunchArgumentStatus::NotSupplied);
}

TEST(SettingsLaunchArgumentsTest, RejectsDuplicateOrUnknownOptions) {
  const auto duplicate =
      azookey::settings::ParseLaunchArguments(L"--langid 0x0411 --langid 0x0409");
  const auto unknown = azookey::settings::ParseLaunchArguments(L"--unknown value");

  EXPECT_EQ(duplicate.langid_status, LaunchArgumentStatus::Invalid);
  EXPECT_TRUE(unknown.parse_error);
}

}  // namespace

// rich-features-spec X-3-6: the TIP opens the batch correction view with a session token only.
TEST(SettingsLaunchArgumentsTest, AcceptsAProofreadSessionToken) {
  const auto arguments = azookey::settings::ParseLaunchArguments(
      L"--proofread-session aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

  EXPECT_FALSE(arguments.parse_error);
  EXPECT_EQ(arguments.proofread_session_status, LaunchArgumentStatus::Valid);
  EXPECT_EQ(arguments.proofread_session_token, std::string(32, 'a'));
  EXPECT_EQ(arguments.langid_status, LaunchArgumentStatus::NotSupplied);
  EXPECT_EQ(arguments.profile_status, LaunchArgumentStatus::NotSupplied);
}

TEST(SettingsLaunchArgumentsTest, RejectsAnythingButALowercaseHexToken) {
  for (const wchar_t* raw : {
           L"--proofread-session",
           L"--proofread-session AAAAAAAAAAAAAAAAaaaaaaaaaaaaaaaa",
           L"--proofread-session aaaaaaaaaaaaaaaa",
           L"--proofread-session aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
           L"--proofread-session ..\\..\\pipe\\aaaaaaaaaaaaaaaaaaaa",
           L"--proofread-session ａaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
           L"--proofread-session aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa "
           L"--proofread-session bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
       }) {
    SCOPED_TRACE(raw);
    const auto arguments = azookey::settings::ParseLaunchArguments(raw);
    EXPECT_TRUE(arguments.parse_error);
    EXPECT_EQ(arguments.proofread_session_status, LaunchArgumentStatus::Invalid);
    EXPECT_TRUE(arguments.proofread_session_token.empty());
  }
}
