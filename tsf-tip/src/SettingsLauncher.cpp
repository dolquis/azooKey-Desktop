#include "azookey/tsf/SettingsLauncher.h"

#include <Objbase.h>
#include <shellapi.h>

#include <array>
#include <filesystem>
#include <new>
#include <string>
#include <system_error>
#ifdef AZOOKEY_TSF_TESTING
#include <utility>
#endif

namespace {

constexpr wchar_t kSettingsExecutable[] = L"azookey_settings.exe";

#ifdef AZOOKEY_TSF_TESTING
std::wstring g_test_executable;
azookey::tsf::testing::ShellExecuteExFn g_test_shell_execute = nullptr;
#endif

std::wstring ResolveSettingsExecutable() {
#ifdef AZOOKEY_TSF_TESTING
  if (!g_test_executable.empty()) {
    return g_test_executable;
  }
#endif

  std::array<wchar_t, 32768> buffer{};
  HMODULE module = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&azookey::tsf::LaunchSettingsApplication), &module)) {
    const DWORD length =
        GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
      const auto adjacent =
          std::filesystem::path(buffer.data()).parent_path() / kSettingsExecutable;
      std::error_code error;
      if (std::filesystem::is_regular_file(adjacent, error)) {
        return adjacent.wstring();
      }
    }
  }

  return {};
}

}  // namespace

namespace azookey::tsf {

HRESULT LaunchSettingsApplication(HWND parent, LANGID langid, REFGUID profile) {
  try {
    const std::wstring executable = ResolveSettingsExecutable();
    if (executable.empty()) {
      return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    std::array<wchar_t, 40> profile_text{};
    if (StringFromGUID2(profile, profile_text.data(), static_cast<int>(profile_text.size())) == 0) {
      return E_INVALIDARG;
    }

    wchar_t langid_text[7]{};
    if (swprintf_s(langid_text, L"0x%04X", static_cast<unsigned int>(langid)) < 0) {
      return E_INVALIDARG;
    }
    const std::wstring parameters =
        L"--langid " + std::wstring(langid_text) + L" --profile " + profile_text.data();
    const std::wstring working_directory =
        std::filesystem::path(executable).parent_path().wstring();

    SHELLEXECUTEINFOW execute_info{};
    execute_info.cbSize = sizeof(execute_info);
    execute_info.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute_info.hwnd = parent;
    execute_info.lpVerb = L"open";
    execute_info.lpFile = executable.c_str();
    execute_info.lpParameters = parameters.c_str();
    execute_info.lpDirectory = working_directory.c_str();
    execute_info.nShow = SW_SHOWNORMAL;

#ifdef AZOOKEY_TSF_TESTING
    const auto shell_execute =
        g_test_shell_execute != nullptr ? g_test_shell_execute : ShellExecuteExW;
#else
    const auto shell_execute = ShellExecuteExW;
#endif
    if (!shell_execute(&execute_info)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    if (execute_info.hProcess != nullptr) {
      CloseHandle(execute_info.hProcess);
    }
    return S_OK;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

#ifdef AZOOKEY_TSF_TESTING
namespace testing {

void SetSettingsLauncherForTest(std::wstring executable, ShellExecuteExFn shell_execute) {
  g_test_executable = std::move(executable);
  g_test_shell_execute = shell_execute;
}

void ClearSettingsLauncherForTest() {
  g_test_executable.clear();
  g_test_shell_execute = nullptr;
}

}  // namespace testing
#endif

}  // namespace azookey::tsf
