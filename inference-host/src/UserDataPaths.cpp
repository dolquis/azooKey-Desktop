#include "azookey/host/UserDataPaths.h"

#include <cstdlib>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <Windows.h>
#include <objbase.h>
#include <KnownFolders.h>
#include <ShlObj.h>
// clang-format on
#endif

namespace azookey::host {

namespace {

bool CreateDirectoryIfNeeded(const std::filesystem::path& path) {
  if (path.empty()) return true;
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) return false;
  return std::filesystem::is_directory(path, ec) && !ec;
}

bool CreateParentDirectoryIfNeeded(const std::filesystem::path& file_path) {
  return CreateDirectoryIfNeeded(file_path.parent_path());
}

}  // namespace

std::optional<std::filesystem::path> GetPlatformLocalAppData() {
#ifdef _WIN32
  PWSTR path = nullptr;
  const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path);
  if (FAILED(hr) || path == nullptr) {
    return std::nullopt;
  }
  std::filesystem::path result(path);
  CoTaskMemFree(path);
  return result;
#else
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
    return std::filesystem::path(xdg);
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / ".local" / "share";
  }
  return std::nullopt;
#endif
}

std::optional<UserDataPaths> ResolveUserDataPaths(const UserDataPathInputs& inputs) {
  const bool have_local = inputs.local_app_data && !inputs.local_app_data->empty();
  const bool have_explicit_all = inputs.explicit_learning_path && inputs.explicit_user_dict_path;

  if (!have_local && !have_explicit_all) {
    // Cannot determine any data paths: fail closed.
    return std::nullopt;
  }

  UserDataPaths paths;
  if (have_local) {
    paths.root_dir = (*inputs.local_app_data / "azooKey").lexically_normal();
    paths.config_dir = (paths.root_dir / "config").lexically_normal();
    paths.data_dir = (paths.root_dir / "data").lexically_normal();
    paths.logs_dir = (paths.root_dir / "logs").lexically_normal();
    paths.models_dir = (paths.root_dir / "models").lexically_normal();
    paths.settings_path = (paths.config_dir / "settings.json").lexically_normal();
  }
  // The data_dir default is only valid when have_local is true.
  paths.learning_path = inputs.explicit_learning_path
                            ? inputs.explicit_learning_path->lexically_normal()
                            : (paths.data_dir / "learning.tsv").lexically_normal();
  paths.user_dict_path = inputs.explicit_user_dict_path
                             ? inputs.explicit_user_dict_path->lexically_normal()
                             : (paths.data_dir / "user_dict.json").lexically_normal();
  return paths;
}

bool EnsureUserDataDirectories(const UserDataPaths& paths) {
  return CreateDirectoryIfNeeded(paths.config_dir) && CreateDirectoryIfNeeded(paths.data_dir) &&
         CreateDirectoryIfNeeded(paths.logs_dir) && CreateDirectoryIfNeeded(paths.models_dir) &&
         CreateParentDirectoryIfNeeded(paths.learning_path) &&
         CreateParentDirectoryIfNeeded(paths.user_dict_path);
}

}  // namespace azookey::host
