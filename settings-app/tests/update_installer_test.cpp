#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "UpdateInstaller.h"

namespace {
using azookey::settings::ClassifyMsiexecExit;
using azookey::settings::IsSha256Hex;
using azookey::settings::MsiexecArguments;
using azookey::settings::UpdateInstallOutcome;
using azookey::settings::UpdatePackagePath;
using azookey::settings::VerifiedUpdatePackage;

// SHA-256 of the three bytes "abc" (FIPS 180-2 example).
constexpr const char* kAbcSha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

class TempDirectory {
 public:
  TempDirectory()
      : path_(std::filesystem::temp_directory_path() /
              ("azookey-update-installer-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

TEST(UpdateInstallerTest, ThePackagePathIsTheHostsDestinationForAPlainVersionOnly) {
  const std::filesystem::path settings = L"C:\\Users\\u\\AppData\\Local\\azooKey\\settings.json";
  const auto path = UpdatePackagePath(settings, "1.2.3");
  ASSERT_TRUE(path);
  EXPECT_EQ(*path, std::filesystem::path(
                       L"C:\\Users\\u\\AppData\\Local\\azooKey\\updates\\azooKey-1.2.3-x64.msi"));
  for (const char* invalid : {"", "v1.2.3", "1.2", "1.2.3.4", "1.2.3-beta.1", "1..3", ".1.2",
                              "1.2.", "..\\..\\x", "1.2.3\"", "1.2.3/x"}) {
    EXPECT_FALSE(UpdatePackagePath(settings, invalid)) << invalid;
  }
  EXPECT_FALSE(UpdatePackagePath({}, "1.2.3"));
  EXPECT_TRUE(IsSha256Hex(kAbcSha));
  EXPECT_FALSE(IsSha256Hex(std::string(64, 'A')));
  EXPECT_FALSE(IsSha256Hex(std::string(63, 'a')));
}

TEST(UpdateInstallerTest, OnlyAFileWithTheExpectedHashOpensAndItStaysLockedWhileOpen) {
  TempDirectory temp;
  const auto file = temp.path() / "azooKey-1.2.3-x64.msi";
  { std::ofstream(file, std::ios::binary) << "abc"; }
  EXPECT_FALSE(VerifiedUpdatePackage::Open(file, std::string(64, 'a')));
  EXPECT_FALSE(VerifiedUpdatePackage::Open(file, "not a hash"));
  EXPECT_FALSE(VerifiedUpdatePackage::Open(temp.path() / "missing.msi", kAbcSha));
  {
    auto package = VerifiedUpdatePackage::Open(file, kAbcSha);
    ASSERT_TRUE(package);
    // The resolved path names the same file (the temporary directory may be given in 8.3 form).
    EXPECT_TRUE(std::filesystem::equivalent(package->path(), file));
    EXPECT_FALSE(package->path().native().starts_with(L"\\\\?\\"));
    // While verified, the file can be read (by msiexec) but not changed, renamed or deleted.
    std::ifstream reader(file, std::ios::binary);
    EXPECT_TRUE(reader.good());
    reader.close();
    HANDLE writer = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_EQ(writer, INVALID_HANDLE_VALUE);
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    EXPECT_FALSE(DeleteFileW(file.c_str()));
    EXPECT_FALSE(MoveFileW(file.c_str(), (temp.path() / "moved.msi").c_str()));
  }
  // Closed: the file is free again.
  std::error_code ec;
  EXPECT_TRUE(std::filesystem::remove(file, ec)) << ec.message();
}

// Renames a directory through its own handle with FILE_RENAME_FLAG_POSIX_SEMANTICS. False when
// the directory cannot be opened for the rename or the rename is refused.
bool PosixRename(const std::filesystem::path& from, const std::filesystem::path& to,
                 DWORD* error = nullptr) {
  HANDLE directory = CreateFileW(from.c_str(), DELETE | SYNCHRONIZE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (directory == INVALID_HANDLE_VALUE) {
    if (error) *error = GetLastError();
    return false;
  }
  const auto& target = to.native();
  std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t));
  auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
  info->Flags = FILE_RENAME_FLAG_POSIX_SEMANTICS;
  info->RootDirectory = nullptr;
  info->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
  std::copy(target.begin(), target.end(), info->FileName);
  const bool renamed = SetFileInformationByHandle(directory, FileRenameInfoEx, info,
                                                  static_cast<DWORD>(buffer.size()));
  if (!renamed && error) *error = GetLastError();
  CloseHandle(directory);
  return renamed;
}

// A junction among the parents is resolved: msiexec gets the real directory, which cannot be
// renamed while the file in it is open, not the junction, which could be re-pointed.
TEST(UpdateInstallerTest, AJunctionAmongTheParentsIsResolvedBeforeMsiexecGetsThePath) {
  TempDirectory temp;
  // real sits one level below an intermediate directory, so both its parent and grandparent of
  // the file can be tried below.
  const auto middle = temp.path() / "middle";
  const auto real = middle / "real";
  const auto link = temp.path() / "link";
  std::filesystem::create_directories(real);
  { std::ofstream(real / "azooKey-1.2.3-x64.msi", std::ios::binary) << "abc"; }
  // A junction (mount point) needs no privilege, unlike a directory symbolic link.
  const std::wstring command =
      L"cmd.exe /c mklink /J \"" + link.native() + L"\" \"" + real.native() + L"\" >nul 2>&1";
  if (_wsystem(command.c_str()) != 0 || !std::filesystem::exists(link / "azooKey-1.2.3-x64.msi")) {
    GTEST_SKIP() << "could not create a junction here";
  }
  {
    auto package = VerifiedUpdatePackage::Open(link / "azooKey-1.2.3-x64.msi", kAbcSha);
    ASSERT_TRUE(package);
    EXPECT_TRUE(std::filesystem::equivalent(package->path().parent_path(), real));
    EXPECT_EQ(package->path().native().find(L"\\link\\"), std::wstring::npos)
        << package->path().string();
    EXPECT_EQ(package->path().filename(), std::filesystem::path(L"azooKey-1.2.3-x64.msi"));
    // The resolved path keeps naming the verified file: while it is open, neither the directory
    // holding it nor that directory's parent can be renamed and replaced.
    EXPECT_FALSE(MoveFileExW(real.c_str(), (middle / "real-moved").c_str(), 0));
    EXPECT_FALSE(MoveFileExW(middle.c_str(), (temp.path() / "middle-moved").c_str(), 0));
    // Nor by the POSIX-semantics rename, which ignores some sharing rules for files.
    EXPECT_FALSE(PosixRename(middle, temp.path() / "middle-moved"));
    EXPECT_TRUE(std::filesystem::exists(real / "azooKey-1.2.3-x64.msi"));
  }
  // Closed: the directories can be renamed again by both ways, so the refusals above came from
  // the open handle. Another process (an antivirus scan of the new file) may hold the tree for a
  // moment, so each rename is retried until it succeeds, for at most 10 seconds.
  const auto eventually = [](const std::function<bool()>& rename) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!rename()) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
  };
  EXPECT_TRUE(eventually([&] {
    return MoveFileExW(middle.c_str(), (temp.path() / "middle-moved").c_str(), 0) != FALSE;
  }));
  DWORD closed_error = 0;
  EXPECT_TRUE(eventually([&] {
    return PosixRename(temp.path() / "middle-moved", middle, &closed_error);
  })) << closed_error;
  std::error_code ec;
  std::filesystem::remove(link, ec);  // Removes the junction, not its target.
}

TEST(UpdateInstallerTest, MsiexecExitCodesAndArgumentsFollowSection63) {
  EXPECT_EQ(ClassifyMsiexecExit(0), UpdateInstallOutcome::Installed);
  EXPECT_EQ(ClassifyMsiexecExit(3010), UpdateInstallOutcome::RebootRequired);
  EXPECT_EQ(ClassifyMsiexecExit(1602), UpdateInstallOutcome::Cancelled);
  EXPECT_EQ(ClassifyMsiexecExit(ERROR_CANCELLED), UpdateInstallOutcome::Cancelled);
  EXPECT_EQ(ClassifyMsiexecExit(1618), UpdateInstallOutcome::Busy);
  EXPECT_EQ(ClassifyMsiexecExit(1603), UpdateInstallOutcome::Failed);
  EXPECT_EQ(MsiexecArguments(L"C:\\u\\updates\\azooKey-1.2.3-x64.msi"),
            L"/i \"C:\\u\\updates\\azooKey-1.2.3-x64.msi\" /qn /norestart");
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

// Every name the pane can show is in the resw: a missing one would show an empty text.
TEST(UpdateInstallerTest, EveryMessageOfTheInstallAreaIsInTheResources) {
  const auto resw = ReadText(std::filesystem::path(std::u8string(u8"" AZOOKEY_SETTINGS_RESW_PATH)));
  const auto has = [&resw](const std::string& name) {
    return resw.find("<data name=\"" + name + "\"") != std::string::npos;
  };
  std::set<std::string> names;
  for (const char* code :
       {"busy", "not_available", "disabled", "cancelled", "network", "asset_missing",
        "digest_missing", "too_large", "download_failed", "save_failed", "something_new"}) {
    names.insert(azookey::settings::UpdateFetchErrorResource(code));
  }
  EXPECT_EQ(azookey::settings::UpdateFetchErrorResource("something_new"),
            "UpdateInstall_Error_Other");
  for (const auto outcome : {UpdateInstallOutcome::Installed, UpdateInstallOutcome::RebootRequired,
                             UpdateInstallOutcome::Cancelled, UpdateInstallOutcome::Busy,
                             UpdateInstallOutcome::Failed}) {
    names.insert(azookey::settings::UpdateInstallOutcomeResource(outcome));
  }
  // And the names MainWindow.xaml.cpp refers to by literal.
  const auto source =
      ReadText(std::filesystem::path(std::u8string(u8"" AZOOKEY_SETTINGS_SOURCE_DIR)) /
               "MainWindow.xaml.cpp");
  const std::regex pattern(R"(L"(UpdateInstall_[A-Za-z0-9_]+)\")");
  size_t literals = 0;
  for (std::sregex_iterator it(source.begin(), source.end(), pattern), end; it != end; ++it) {
    names.insert((*it)[1].str());
    ++literals;
  }
  EXPECT_GT(literals, 0u);
  for (const auto& name : names) EXPECT_TRUE(has(name)) << name;
}

}  // namespace
