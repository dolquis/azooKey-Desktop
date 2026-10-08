#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace azookey::host {

struct UserDataPathInputs {
  std::optional<std::filesystem::path> local_app_data;
  std::optional<std::filesystem::path> explicit_root_dir;
  std::optional<std::filesystem::path> explicit_learning_path;
  std::optional<std::filesystem::path> explicit_user_dict_path;
};

struct UserDataPaths {
  std::filesystem::path root_dir;
  std::filesystem::path config_dir;
  std::filesystem::path data_dir;
  std::filesystem::path logs_dir;
  std::filesystem::path models_dir;
  // Optional download packs (auto-word-registration-spec section 14.10). Created only when a
  // pack is fetched, so EnsureUserDataDirectories leaves it alone.
  std::filesystem::path packs_dir;
  std::filesystem::path settings_path;
  std::filesystem::path learning_path;
  std::filesystem::path user_dict_path;
  // M35 / M36-A stores. Placed beside learning.tsv rather than under data_dir
  // directly, so an explicit --learning with no LOCALAPPDATA still puts
  // them somewhere real.
  std::filesystem::path typo_store_path;
  std::filesystem::path auto_word_store_path;
  // M60 English learning channel: the LearningStore format in its own file so
  // English commits never mix with kana-kanji learning.
  std::filesystem::path english_learning_path;
};

std::optional<std::filesystem::path> GetPlatformLocalAppData();
std::optional<UserDataPaths> ResolveUserDataPaths(const UserDataPathInputs& inputs);
bool EnsureUserDataDirectories(const UserDataPaths& paths);

// UTF-8 path with an optional leading "%LOCALAPPDATA%" (case-insensitive)
// expanded. nullopt when the prefix is present but LOCALAPPDATA is unknown.
std::optional<std::filesystem::path> ExpandLocalAppDataPrefix(std::string_view utf8_path);

}  // namespace azookey::host
