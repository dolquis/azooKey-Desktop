#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace azookey::settings {

// DEV-1974 (docs/sideload-packaging-spec.md section 6.3): installing the MSI the Host downloaded
// and verified. The settings app re-verifies it under a handle that denies writes and deletion,
// keeps that handle until msiexec ends, and asks Windows for the elevation (UAC), which is the
// user's approval.

// <settings dir>/updates/azooKey-<version>-x64.msi, the Host's download destination. nullopt
// unless version is MAJOR.MINOR.PATCH (digits and two dots only), so nothing a pipe peer sends
// can name another file.
std::optional<std::filesystem::path> UpdatePackagePath(const std::filesystem::path& settings_path,
                                                       std::string_view version);

// A lowercase hex SHA-256.
bool IsSha256Hex(std::string_view text);

// The package opened for reading with only FILE_SHARE_READ: while it lives, nobody can write,
// rename or delete the file, and Windows Installer can still read it.
class VerifiedUpdatePackage {
 public:
  // nullopt when the file cannot be opened or its SHA-256 is not expected_sha256.
  static std::optional<VerifiedUpdatePackage> Open(const std::filesystem::path& path,
                                                   std::string_view expected_sha256);
  VerifiedUpdatePackage(VerifiedUpdatePackage&& other) noexcept;
  VerifiedUpdatePackage& operator=(VerifiedUpdatePackage&& other) noexcept;
  VerifiedUpdatePackage(const VerifiedUpdatePackage&) = delete;
  VerifiedUpdatePackage& operator=(const VerifiedUpdatePackage&) = delete;
  ~VerifiedUpdatePackage();

  const std::filesystem::path& path() const { return path_; }

 private:
  VerifiedUpdatePackage(std::filesystem::path path, HANDLE handle);

  std::filesystem::path path_;
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

enum class UpdateInstallOutcome : uint8_t {
  Installed,       // 0.
  RebootRequired,  // 3010 (ERROR_SUCCESS_REBOOT_REQUIRED): installed; a restart finishes it.
  Cancelled,       // The UAC prompt was declined, or 1602 (ERROR_INSTALL_USEREXIT).
  Busy,            // 1618 (ERROR_INSTALL_ALREADY_RUNNING).
  Failed,
};

UpdateInstallOutcome ClassifyMsiexecExit(DWORD code);

// Resource names for the "バージョン" pane: a refusal ("busy", "not_available", ...) or a result
// error ("digest_missing", "cancelled", ...) of the Host's download, and the install outcome.
std::string UpdateFetchErrorResource(std::string_view code);
std::string UpdateInstallOutcomeResource(UpdateInstallOutcome outcome);

// /i "<package>" /qn /norestart: no UI and no automatic restart (section 6.3). No log option, so
// the elevated process writes nowhere the user can.
std::wstring MsiexecArguments(const std::filesystem::path& package);

struct UpdateInstallResult {
  UpdateInstallOutcome outcome{UpdateInstallOutcome::Failed};
  DWORD code{0};  // msiexec's exit code, or the launch error.
};

// Runs %SystemRoot%\System32\msiexec.exe elevated and waits for it. Blocks: call it off the UI
// thread. The package stays open, so the file msiexec reads is the verified one.
UpdateInstallResult RunElevatedMsiexec(const VerifiedUpdatePackage& package, HWND owner);

// start-installed-host.ps1 in HKLM\SOFTWARE\azooKey\HostStartup's InstallDirectory, the launcher
// the MSI's Run entry uses. nullopt when the key or the script is missing.
std::optional<std::filesystem::path> InstalledHostLauncher();
// Starts that launcher as this user, hidden, as the Run entry does at sign-in. The installation
// stopped the Host, which the Run entry would bring back only at the next sign-in.
bool StartInstalledHost();
// HKLM\SOFTWARE\azooKey\VulkanAddon: the add-on, which this update leaves at the old version.
bool VulkanAddonInstalled();
// Restarts Windows (after the user agreed, for RebootRequired).
bool RequestRestart();

}  // namespace azookey::settings
