#include "azookey/tsf/ForegroundAppDetector.h"

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

}  // namespace

bool WindowsAppNameEqual(std::string_view left, std::string_view right) {
  return core::EqualAppName(left, right);
}

core::ForegroundApp ForegroundAppDetector::Get() {
#ifdef AZOOKEY_TSF_TESTING
  if (test_app_) return *test_app_;
#endif
  // An in-process TIP identifies the app receiving these keys, not a shell
  // frame or unrelated overlay that happens to own the foreground window.
  if (cached_.process_name.empty()) {
    std::array<WCHAR, 32768> path{};
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return {};
    const std::wstring_view full(path.data(), count);
    const auto slash = full.find_last_of(L"\\/");
    cached_.process_name = Utf8(full.substr(slash == std::wstring_view::npos ? 0 : slash + 1));
  }
  cached_.window_class.clear();
  HWND window = GetFocus();
  DWORD pid = 0;
  if (window) GetWindowThreadProcessId(window, &pid);
  if (pid != GetCurrentProcessId()) window = nullptr;
  if (window) {
    const HWND root = GetAncestor(window, GA_ROOT);
    DWORD root_pid = 0;
    if (root) GetWindowThreadProcessId(root, &root_pid);
    if (root_pid == GetCurrentProcessId()) window = root;
  }
  std::array<WCHAR, 256> window_class{};
  const int class_length =
      window ? GetClassNameW(window, window_class.data(), static_cast<int>(window_class.size()))
             : 0;
  if (class_length > 0)
    cached_.window_class = Utf8({window_class.data(), static_cast<size_t>(class_length)});
  cached_.resolved = !cached_.process_name.empty();
  return cached_;
}
}  // namespace azookey::tsf
