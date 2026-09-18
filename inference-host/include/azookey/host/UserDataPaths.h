#pragma once

#include <filesystem>
#include <optional>

namespace azookey::host {

struct UserDataPathInputs {
  std::optional<std::filesystem::path> local_app_data;
  std::optional<std::filesystem::path> explicit_learning_path;
  std::optional<std::filesystem::path> explicit_user_dict_path;
};

struct UserDataPaths {
  std::filesystem::path root_dir;
  std::filesystem::path config_dir;
  std::filesystem::path data_dir;
  std::filesystem::path logs_dir;
  std::filesystem::path models_dir;
  std::filesystem::path settings_path;
  std::filesystem::path learning_path;
  std::filesystem::path user_dict_path;
  // M35 / M36-A stores. Placed beside learning.tsv rather than under data_dir
  // directly, so an explicit --learning-path with no LOCALAPPDATA still puts
  // them somewhere real.
  std::filesystem::path typo_store_path;
  std::filesystem::path auto_word_store_path;
};

std::optional<std::filesystem::path> GetPlatformLocalAppData();
std::optional<UserDataPaths> ResolveUserDataPaths(const UserDataPathInputs& inputs);
bool EnsureUserDataDirectories(const UserDataPaths& paths);

}  // namespace azookey::host
