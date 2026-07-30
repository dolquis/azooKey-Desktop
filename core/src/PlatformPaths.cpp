#include "azookey/core/PlatformPaths.h"

#include <cstdlib>
#include <memory>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>
// clang-format on
#endif

namespace azookey::core {

std::optional<std::filesystem::path> GetLocalAppDataDirectory() noexcept {
  try {
#ifdef _WIN32
    PWSTR path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path);
    if (FAILED(result) || path == nullptr) return std::nullopt;

    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_path(path, &CoTaskMemFree);
    return std::filesystem::path(owned_path.get());
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
      return std::filesystem::path(xdg);
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
      return std::filesystem::path(home) / ".local" / "share";
    }
    return std::nullopt;
#endif
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace azookey::core
