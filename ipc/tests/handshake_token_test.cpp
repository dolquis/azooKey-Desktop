#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "azookey/ipc/HandshakeToken.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <aclapi.h>
#endif

namespace azookey::ipc {
namespace {

class HandshakeTokenTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 ("azookey-token-test-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    path_ = directory_ / "ipc-token";
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(directory_, ec);
  }
  std::filesystem::path directory_;
  std::filesystem::path path_;
};

TEST_F(HandshakeTokenTest, PublishAndReadRoundTrip) {
  const auto first = GenerateHandshakeToken();
  const auto second = GenerateHandshakeToken();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(*first, *second);
  EXPECT_EQ(first->size(), 32u);
  ASSERT_TRUE(PublishHandshakeToken(path_, *first));
  EXPECT_EQ(ReadHandshakeTokenFile(path_), first);
  ASSERT_TRUE(PublishHandshakeToken(path_, *second));
  EXPECT_EQ(ReadHandshakeTokenFile(path_), second);
}

TEST_F(HandshakeTokenTest, MissingAndMalformedFilesAreRejected) {
  EXPECT_FALSE(ReadHandshakeTokenFile(path_));
  std::filesystem::create_directories(directory_);
  const std::vector<std::string> malformed = {"", "deadbeef", std::string(32, 'g'),
                                              std::string(32, 'a') + "\n"};
  for (const auto& value : malformed) {
    std::ofstream(path_, std::ios::binary | std::ios::trunc) << value;
    EXPECT_FALSE(ReadHandshakeTokenFile(path_)) << value.size();
  }
  EXPECT_FALSE(PublishHandshakeToken(path_, "invalid"));
}

TEST_F(HandshakeTokenTest, PublishReportsRenameFailureAndRemovesTemporaryFile) {
  // A non-empty directory at the destination makes the final rename fail on
  // both POSIX (EISDIR) and Windows (MoveFileExW access denied).
  std::filesystem::create_directories(path_);
  std::ofstream(path_ / "occupied", std::ios::binary) << "x";

  EXPECT_FALSE(PublishHandshakeToken(path_, "0123456789abcdef0123456789abcdef"));
  EXPECT_TRUE(std::filesystem::is_directory(path_));
  for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
    EXPECT_EQ(entry.path(), path_) << "leftover: " << entry.path().string();
  }

  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
}

#ifdef _WIN32
TEST_F(HandshakeTokenTest, PublishedFileHasProtectedPrivateDacl) {
  ASSERT_TRUE(PublishHandshakeToken(path_, "0123456789abcdef0123456789abcdef"));
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  PACL dacl = nullptr;
  ASSERT_EQ(GetNamedSecurityInfoW(path_.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                  nullptr, &dacl, nullptr, &descriptor),
            ERROR_SUCCESS);
  SECURITY_DESCRIPTOR_CONTROL control{};
  DWORD revision = 0;
  ASSERT_TRUE(GetSecurityDescriptorControl(descriptor, &control, &revision));
  EXPECT_NE(control & SE_DACL_PROTECTED, 0);
  ASSERT_NE(dacl, nullptr);
  ACL_SIZE_INFORMATION info{};
  ASSERT_TRUE(GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation));
  EXPECT_EQ(info.AceCount, 3u);
  HANDLE process_token = nullptr;
  ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token));
  DWORD required = 0;
  GetTokenInformation(process_token, TokenUser, nullptr, 0, &required);
  std::vector<unsigned char> token_info(required);
  ASSERT_TRUE(
      GetTokenInformation(process_token, TokenUser, token_info.data(), required, &required));
  CloseHandle(process_token);
  const auto* user = reinterpret_cast<const TOKEN_USER*>(token_info.data());
  for (DWORD i = 0; i < info.AceCount; ++i) {
    void* raw_ace = nullptr;
    ASSERT_TRUE(GetAce(dacl, i, &raw_ace));
    auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw_ace);
    ASSERT_EQ(ace->Header.AceType, ACCESS_ALLOWED_ACE_TYPE);
    auto* ace_sid = &ace->SidStart;
    EXPECT_TRUE(EqualSid(ace_sid, user->User.Sid) || IsWellKnownSid(ace_sid, WinLocalSystemSid) ||
                IsWellKnownSid(ace_sid, WinBuiltinAdministratorsSid));
  }
  LocalFree(descriptor);
}
#endif

}  // namespace
}  // namespace azookey::ipc
