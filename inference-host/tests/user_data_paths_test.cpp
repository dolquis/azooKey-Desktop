#include <gtest/gtest.h>

#include <filesystem>

#include "azookey/host/UserDataPaths.h"

namespace {

std::filesystem::path TestRoot(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

}  // namespace

TEST(UserDataPathsTest, DefaultPathsUseLocalAppDataLayout) {
  const auto local = TestRoot("azookey_m39_localappdata");
  azookey::host::UserDataPathInputs inputs;
  inputs.local_app_data = local;

  auto paths = azookey::host::ResolveUserDataPaths(inputs);
  ASSERT_TRUE(paths.has_value());
  EXPECT_EQ(paths->root_dir, local / "azooKey");
  EXPECT_EQ(paths->config_dir, local / "azooKey" / "config");
  EXPECT_EQ(paths->data_dir, local / "azooKey" / "data");
  EXPECT_EQ(paths->logs_dir, local / "azooKey" / "logs");
  EXPECT_EQ(paths->models_dir, local / "azooKey" / "models");
  EXPECT_EQ(paths->settings_path, local / "azooKey" / "config" / "settings.json");
  EXPECT_EQ(paths->learning_path, local / "azooKey" / "data" / "learning.tsv");
  EXPECT_EQ(paths->user_dict_path, local / "azooKey" / "data" / "user_dict.json");
}

TEST(UserDataPathsTest, ExplicitPathsOverrideDefaults) {
  const auto local = TestRoot("azookey_m39_localappdata");
  const auto explicit_learning = TestRoot("azookey_m39_explicit") / "learning.tsv";
  const auto explicit_user_dict = TestRoot("azookey_m39_explicit") / "user-dict.json";
  azookey::host::UserDataPathInputs inputs;
  inputs.local_app_data = local;
  inputs.explicit_learning_path = explicit_learning;
  inputs.explicit_user_dict_path = explicit_user_dict;

  auto paths = azookey::host::ResolveUserDataPaths(inputs);
  ASSERT_TRUE(paths.has_value());
  EXPECT_EQ(paths->learning_path, explicit_learning);
  EXPECT_EQ(paths->user_dict_path, explicit_user_dict);
}

TEST(UserDataPathsTest, MissingLocalAppDataFailsClosed) {
  azookey::host::UserDataPathInputs inputs;
  EXPECT_FALSE(azookey::host::ResolveUserDataPaths(inputs).has_value());
}

TEST(UserDataPathsTest, FullyExplicitPathsWorkWithoutLocalAppData) {
  const auto learning = TestRoot("azookey_explicit_only") / "learn.tsv";
  const auto user_dict = TestRoot("azookey_explicit_only") / "dict.json";
  azookey::host::UserDataPathInputs inputs;
  // local_app_data intentionally absent
  inputs.explicit_learning_path = learning;
  inputs.explicit_user_dict_path = user_dict;

  auto paths = azookey::host::ResolveUserDataPaths(inputs);
  ASSERT_TRUE(paths.has_value());
  EXPECT_EQ(paths->learning_path, learning);
  EXPECT_EQ(paths->user_dict_path, user_dict);
  // Directory fields are empty (no LocalAppData to derive them from).
  EXPECT_TRUE(paths->root_dir.empty());
  EXPECT_TRUE(paths->settings_path.empty());
}

TEST(UserDataPathsTest, EnsureCreatesLayoutAndExplicitParents) {
  const auto local = TestRoot("azookey_m39_layout");
  const auto explicit_dir = TestRoot("azookey_m39_explicit_parent") / "nested";
  std::filesystem::remove_all(local);
  std::filesystem::remove_all(explicit_dir.parent_path());

  azookey::host::UserDataPathInputs inputs;
  inputs.local_app_data = local;
  inputs.explicit_learning_path = explicit_dir / "learning.tsv";

  auto paths = azookey::host::ResolveUserDataPaths(inputs);
  ASSERT_TRUE(paths.has_value());
  EXPECT_TRUE(azookey::host::EnsureUserDataDirectories(*paths));
  EXPECT_TRUE(std::filesystem::is_directory(paths->config_dir));
  EXPECT_TRUE(std::filesystem::is_directory(paths->data_dir));
  EXPECT_TRUE(std::filesystem::is_directory(paths->logs_dir));
  EXPECT_TRUE(std::filesystem::is_directory(paths->models_dir));
  EXPECT_TRUE(std::filesystem::is_directory(explicit_dir));

  std::filesystem::remove_all(local);
  std::filesystem::remove_all(explicit_dir.parent_path());
}
