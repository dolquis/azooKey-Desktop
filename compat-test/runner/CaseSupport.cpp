#include "runner/CaseSupport.h"

#include <ShlObj.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "azookey/ipc/Json.h"

namespace azookey::compat_test {

bool IsRectWithinBounds(const RECT& rect, const RECT& bounds) {
  return rect.left >= bounds.left && rect.top >= bounds.top && rect.right <= bounds.right &&
         rect.bottom <= bounds.bottom && rect.right > rect.left && rect.bottom > rect.top;
}

bool IsCandidateNearCaretAtDpi(const RECT& candidate, const RECT& caret, UINT dpi) {
  if (dpi == 0) return false;
  const LONG horizontal_tolerance = MulDiv(64, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
  const LONG maximum_vertical_gap = MulDiv(100, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
  const LONG top_tolerance = MulDiv(4, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
  return std::abs(candidate.left - caret.left) <= horizontal_tolerance &&
         candidate.top >= caret.bottom - top_tolerance &&
         candidate.top - caret.bottom <= maximum_vertical_gap;
}

bool IsExpectedC002BackspaceTransition(std::wstring_view before, std::wstring_view after) {
  return before == C002BackspaceScenario::kBefore && before != after &&
         (after == C002BackspaceScenario::kExpectedAfter[0] ||
          after == C002BackspaceScenario::kExpectedAfter[1]);
}

LiveConversionSetting ReadLiveConversionSetting() {
  PWSTR local_app_data = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data)) ||
      !local_app_data)
    return LiveConversionSetting::Unavailable;
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_path(local_app_data,
                                                                      &CoTaskMemFree);
  const auto path =
      std::filesystem::path(owned_path.get()) / L"azooKey" / L"config" / L"settings.json";
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
               ? LiveConversionSetting::Disabled
               : LiveConversionSetting::Unavailable;
  }
  LARGE_INTEGER size{};
  constexpr LONGLONG kLimit = 1024 * 1024;
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > kLimit) {
    CloseHandle(file);
    return LiveConversionSetting::Unavailable;
  }
  std::string contents(static_cast<size_t>(size.QuadPart) + 1, '\0');
  DWORD count = 0;
  const BOOL read =
      ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &count, nullptr);
  CloseHandle(file);
  if (!read || count > static_cast<size_t>(size.QuadPart))
    return LiveConversionSetting::Unavailable;
  contents.resize(count);
  const auto json = ipc::json::Parse(contents);
  if (!json || !json->IsObject()) return LiveConversionSetting::Disabled;
  return json->GetBool("liveConversion").value_or(false) ? LiveConversionSetting::Enabled
                                                         : LiveConversionSetting::Disabled;
}

}  // namespace azookey::compat_test
