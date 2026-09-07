#include "azookey/tsf/ForegroundAppDetector.h"

#include <algorithm>
#include <array>

namespace azookey::tsf {
namespace {
std::string Utf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int count =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                          nullptr, 0, nullptr, nullptr);
  if (!count) return {};
  std::string result(count, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                      result.data(), count, nullptr, nullptr);
  return result;
}

std::wstring Wide(std::string_view text) {
  if (text.empty() || text.size() > 32768) return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
  if (!count) return {};
  std::wstring result(count, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                      result.data(), count);
  return result;
}
}  // namespace

bool WindowsAppNameEqual(std::string_view left, std::string_view right) {
  const auto ascii = [](std::string_view name) {
    return std::all_of(name.begin(), name.end(), [](unsigned char ch) { return ch < 128; });
  };
  if (ascii(left) && ascii(right)) return core::EqualAsciiAppName(left, right);
  const auto a = Wide(left), b = Wide(right);
  return !a.empty() && !b.empty() &&
         CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                              static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

core::ForegroundApp ForegroundAppDetector::Get() {
#ifdef AZOOKEY_TSF_TESTING
  if (test_app_) return *test_app_;
#endif
  const HWND window = GetForegroundWindow();
  DWORD pid = 0;
  if (window) GetWindowThreadProcessId(window, &pid);
  const auto now = GetTickCount64();
  if (window == window_ && pid == process_id_ && now < expires_) return cached_;
  cached_ = {};
  window_ = window;
  process_id_ = pid;
  expires_ = now + 500;
  if (!window || !pid) return cached_;
  std::array<WCHAR, 32768> path{};
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return cached_;
  DWORD count = static_cast<DWORD>(path.size());
  const BOOL queried = QueryFullProcessImageNameW(process, 0, path.data(), &count);
  CloseHandle(process);
  if (!queried || !count) return cached_;
  std::wstring_view full(path.data(), count);
  const auto slash = full.find_last_of(L"\\/");
  auto name = full.substr(slash == std::wstring_view::npos ? 0 : slash + 1);
  cached_.process_name = Utf8(name);
  std::array<WCHAR, 256> window_class{};
  const int class_length =
      GetClassNameW(window, window_class.data(), static_cast<int>(window_class.size()));
  if (class_length > 0)
    cached_.window_class = Utf8({window_class.data(), static_cast<size_t>(class_length)});
  // A focus switch during process querying invalidates this observation.
  DWORD current_pid = 0;
  if (GetForegroundWindow() == window) GetWindowThreadProcessId(window, &current_pid);
  if (current_pid != pid) {
    Invalidate();
    cached_ = {};
    return cached_;
  }
  cached_.resolved = !cached_.process_name.empty();
  return cached_;
}
}  // namespace azookey::tsf
