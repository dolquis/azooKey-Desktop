#include "UpdateInstaller.h"

#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace azookey::settings {
namespace {

constexpr DWORD kRebootRequired = 3010;  // ERROR_SUCCESS_REBOOT_REQUIRED
constexpr DWORD kUserExit = 1602;        // ERROR_INSTALL_USEREXIT
constexpr DWORD kAlreadyRunning = 1618;  // ERROR_INSTALL_ALREADY_RUNNING

std::optional<std::filesystem::path> SystemDirectory() {
  std::array<wchar_t, MAX_PATH> buffer{};
  const UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return std::nullopt;
  return std::filesystem::path(buffer.data());
}

bool HasQuote(const std::filesystem::path& path) {
  return path.native().find(L'"') != std::wstring::npos;
}

// The final path of an open file in the drive-letter form ("C:\..."), or "\\server\share\..."
// for a network file, without the "\\?\" prefix msiexec does not need.
std::optional<std::filesystem::path> ResolvedPathOf(HANDLE file) {
  std::wstring buffer(MAX_PATH, L'\0');
  for (int attempt = 0; attempt < 2; ++attempt) {
    const DWORD length =
        GetFinalPathNameByHandleW(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0) return std::nullopt;
    if (length < buffer.size()) {
      buffer.resize(length);
      constexpr std::wstring_view kUnc = L"\\\\?\\UNC\\";
      constexpr std::wstring_view kLocal = L"\\\\?\\";
      if (buffer.starts_with(kUnc))
        return std::filesystem::path(L"\\\\" + buffer.substr(kUnc.size()));
      if (buffer.starts_with(kLocal)) return std::filesystem::path(buffer.substr(kLocal.size()));
      return std::filesystem::path(buffer);
    }
    buffer.assign(length, L'\0');  // Too small: length is the size it needs.
  }
  return std::nullopt;
}

std::optional<std::string> HashOpenFile(HANDLE file) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (!BCRYPT_SUCCESS(
          BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    return std::nullopt;
  }
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::optional<std::string> hex;
  if (BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
    std::vector<unsigned char> buffer(1 << 20);
    bool ok = true;
    for (;;) {
      DWORD read = 0;
      if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
        ok = false;
        break;
      }
      if (read == 0) break;
      if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), read, 0))) {
        ok = false;
        break;
      }
    }
    std::array<unsigned char, 32> digest{};
    if (ok && BCRYPT_SUCCESS(
                  BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
      constexpr char kHex[] = "0123456789abcdef";
      std::string text;
      text.reserve(64);
      for (const auto byte : digest) {
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 0x0f]);
      }
      hex = std::move(text);
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return hex;
}

}  // namespace

std::optional<std::filesystem::path> UpdatePackagePath(const std::filesystem::path& settings_path,
                                                       std::string_view version) {
  // Three non-empty runs of digits, as release.yml tags (vMAJOR.MINOR.PATCH) and names the MSI.
  int dots = 0;
  bool digit_run = false;
  if (version.empty() || version.size() > 32) return std::nullopt;
  for (const char c : version) {
    if (c >= '0' && c <= '9') {
      digit_run = true;
    } else if (c == '.' && digit_run) {
      ++dots;
      digit_run = false;
    } else {
      return std::nullopt;
    }
  }
  if (dots != 2 || !digit_run || settings_path.empty()) return std::nullopt;
  return settings_path.parent_path() / L"updates" /
         (L"azooKey-" + std::wstring(version.begin(), version.end()) + L"-x64.msi");
}

bool IsSha256Hex(std::string_view text) {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

VerifiedUpdatePackage::VerifiedUpdatePackage(std::filesystem::path path, HANDLE handle)
    : path_(std::move(path)), handle_(handle) {}

VerifiedUpdatePackage::VerifiedUpdatePackage(VerifiedUpdatePackage&& other) noexcept
    : path_(std::move(other.path_)), handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

VerifiedUpdatePackage& VerifiedUpdatePackage::operator=(VerifiedUpdatePackage&& other) noexcept {
  if (this != &other) {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    path_ = std::move(other.path_);
    handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
  }
  return *this;
}

VerifiedUpdatePackage::~VerifiedUpdatePackage() {
  if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
}

std::optional<VerifiedUpdatePackage> VerifiedUpdatePackage::Open(const std::filesystem::path& path,
                                                                 std::string_view expected_sha256) {
  if (!IsSha256Hex(expected_sha256) || HasQuote(path)) return std::nullopt;
  // FILE_FLAG_OPEN_REPARSE_POINT opens a link itself, which is then refused: the handle must pin
  // the very file msiexec opens by this path, and a link could be pointed elsewhere afterwards.
  HANDLE file = CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) return std::nullopt;
  VerifiedUpdatePackage package(path, file);
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (GetFileType(file) != FILE_TYPE_DISK ||
      !GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) ||
      (tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
    return std::nullopt;
  }
  // A junction among the parent directories could be pointed elsewhere after the check, so
  // msiexec gets the path of the opened file itself, every junction resolved. A directory that
  // holds an open file cannot be renamed, so that path keeps naming this file.
  const auto resolved = ResolvedPathOf(file);
  if (!resolved || HasQuote(*resolved) ||
      _wcsicmp(resolved->filename().c_str(), path.filename().c_str()) != 0) {
    return std::nullopt;
  }
  package.path_ = *resolved;
  const auto actual = HashOpenFile(file);
  if (!actual || *actual != expected_sha256) return std::nullopt;
  return package;
}

UpdateInstallOutcome ClassifyMsiexecExit(DWORD code) {
  switch (code) {
    case ERROR_SUCCESS:
      return UpdateInstallOutcome::Installed;
    case kRebootRequired:
      return UpdateInstallOutcome::RebootRequired;
    case kUserExit:
    case ERROR_CANCELLED:
      return UpdateInstallOutcome::Cancelled;
    case kAlreadyRunning:
      return UpdateInstallOutcome::Busy;
    default:
      return UpdateInstallOutcome::Failed;
  }
}

std::string UpdateFetchErrorResource(std::string_view code) {
  for (const std::string_view known :
       {"busy", "not_available", "disabled", "cancelled", "network", "asset_missing",
        "digest_missing", "too_large", "download_failed", "save_failed"}) {
    if (code == known) return "UpdateInstall_Error_" + std::string(code);
  }
  return "UpdateInstall_Error_Other";
}

std::string UpdateInstallOutcomeResource(UpdateInstallOutcome outcome) {
  switch (outcome) {
    case UpdateInstallOutcome::Installed:
      return "UpdateInstall_Installed";
    case UpdateInstallOutcome::RebootRequired:
      return "UpdateInstall_RebootPending";
    case UpdateInstallOutcome::Cancelled:
      return "UpdateInstall_Cancelled";
    case UpdateInstallOutcome::Busy:
      return "UpdateInstall_Busy";
    case UpdateInstallOutcome::Failed:
      break;
  }
  return "UpdateInstall_Failed";
}

std::wstring MsiexecArguments(const std::filesystem::path& package) {
  return L"/i \"" + package.native() + L"\" /qn /norestart";
}

UpdateInstallResult RunElevatedMsiexec(const VerifiedUpdatePackage& package, HWND owner) {
  UpdateInstallResult result;
  const auto system = SystemDirectory();
  if (!system || HasQuote(package.path())) {
    result.code = ERROR_PATH_NOT_FOUND;
    return result;
  }
  const auto msiexec = (*system / L"msiexec.exe").native();
  const auto arguments = MsiexecArguments(package.path());
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  info.hwnd = owner;
  info.lpVerb = L"runas";
  info.lpFile = msiexec.c_str();
  info.lpParameters = arguments.c_str();
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info) || !info.hProcess) {
    result.code = GetLastError();
    result.outcome = result.code == ERROR_CANCELLED ? UpdateInstallOutcome::Cancelled
                                                    : UpdateInstallOutcome::Failed;
    return result;
  }
  WaitForSingleObject(info.hProcess, INFINITE);
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(info.hProcess, &exit_code)) exit_code = GetLastError();
  CloseHandle(info.hProcess);
  result.code = exit_code;
  result.outcome = ClassifyMsiexecExit(exit_code);
  return result;
}

std::optional<std::filesystem::path> InstalledHostLauncher() {
  std::array<wchar_t, 1024> buffer{};
  DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
  // The MSI writes the key from a 64-bit component (Bitness="always64").
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\azooKey\\HostStartup", L"InstallDirectory",
                   RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer.data(),
                   &size) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  const std::filesystem::path directory(buffer.data());
  if (!directory.is_absolute() || HasQuote(directory)) return std::nullopt;
  const auto launcher = directory / L"start-installed-host.ps1";
  std::error_code ec;
  if (!std::filesystem::is_regular_file(launcher, ec) || ec) return std::nullopt;
  return launcher;
}

bool StartInstalledHost() {
  const auto launcher = InstalledHostLauncher();
  const auto system = SystemDirectory();
  if (!launcher || !system) return false;
  // The same command as the MSI's Run entry (pkg/msi/Package.wxs), as this user.
  const auto powershell = (*system / L"WindowsPowerShell\\v1.0\\powershell.exe").native();
  std::wstring command = L"\"" + powershell +
                         L"\" -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy "
                         L"Bypass -File \"" +
                         launcher->native() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

bool VulkanAddonInstalled() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\azooKey\\VulkanAddon", 0,
                    KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return false;
  }
  RegCloseKey(key);
  return true;
}

bool RequestRestart() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    return false;
  }
  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  const bool adjusted =
      LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid) &&
      AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) &&
      GetLastError() == ERROR_SUCCESS;
  CloseHandle(token);
  if (!adjusted) return false;
  return InitiateShutdownW(nullptr, nullptr, 0, SHUTDOWN_RESTART,
                           SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_INSTALLATION |
                               SHTDN_REASON_FLAG_PLANNED) == ERROR_SUCCESS;
}

}  // namespace azookey::settings
