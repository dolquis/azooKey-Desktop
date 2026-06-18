#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "azookey/host/SettingsStore.h"

namespace {

std::filesystem::path TestDir(const char* name) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << text;
}

}  // namespace

TEST(SettingsStoreTest, MissingFileUsesSchemaDefaults) {
  const auto dir = TestDir("azookey_settings_missing");
  const auto path = dir / "settings.json";
  azookey::host::SettingsStore store(path);

  const auto result = store.Load();

  EXPECT_EQ(result.status, azookey::host::SettingsLoadStatus::Missing);
  EXPECT_EQ(result.settings.input_mode, "hiragana");
  EXPECT_FALSE(result.settings.live_conversion);
  EXPECT_TRUE(result.settings.prediction_enabled);
  EXPECT_EQ(result.settings.backend_preference, "auto");
  EXPECT_TRUE(result.settings.model.enabled);
  EXPECT_TRUE(result.settings.model.auto_load_on_host_start);

  std::filesystem::remove_all(dir);
}

TEST(SettingsStoreTest, PartialFileFillsDefaultsAndAppliesEngineConfig) {
  const auto dir = TestDir("azookey_settings_partial");
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "liveConversion": true,
    "predictionEnabled": false,
    "backendPreference": "cuda",
    "model": {
      "selectedPath": "C:/models/zenz-v3.gguf",
      "nGpuLayers": 12
    }
  })");

  azookey::host::SettingsStore store(path);
  const auto result = store.Load();

  EXPECT_EQ(result.status, azookey::host::SettingsLoadStatus::Loaded);
  EXPECT_EQ(result.settings.input_mode, "hiragana");
  EXPECT_TRUE(result.settings.live_conversion);
  EXPECT_FALSE(result.settings.prediction_enabled);
  EXPECT_EQ(result.settings.backend_preference, "cuda");
  EXPECT_EQ(result.settings.model.selected_path, "C:/models/zenz-v3.gguf");
  EXPECT_EQ(result.settings.model.n_gpu_layers, 12);
  EXPECT_TRUE(result.settings.model.auto_load_on_host_start);

  azookey::host::EngineConfig config;
  config = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, result.settings);
  EXPECT_TRUE(config.enable_live_conversion);
  EXPECT_EQ(config.backend, azookey::host::BackendKind::Cuda);
  EXPECT_EQ(config.model_path, "C:/models/zenz-v3.gguf");
  ASSERT_TRUE(config.n_gpu_layers.has_value());
  EXPECT_EQ(*config.n_gpu_layers, 12);

  std::filesystem::remove_all(dir);
}

TEST(SettingsStoreTest, ModelBlockOverridesRootBackendAndCanDisableModel) {
  const auto dir = TestDir("azookey_settings_model_override");
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "backendPreference": "cuda",
    "model": {
      "enabled": false,
      "backendPreference": "cpu",
      "selectedPath": "C:/models/ignored.gguf",
      "autoLoadOnHostStart": false
    }
  })");

  azookey::host::SettingsStore store(path);
  const auto result = store.Load();
  azookey::host::EngineConfig config;
  config.model_path = "C:/models/existing.gguf";
  config = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, result.settings);

  EXPECT_FALSE(result.settings.model.enabled);
  EXPECT_FALSE(result.settings.model.auto_load_on_host_start);
  EXPECT_EQ(config.backend, azookey::host::BackendKind::Cpu);
  EXPECT_TRUE(config.model_path.empty());
  EXPECT_FALSE(config.n_gpu_layers.has_value());

  std::filesystem::remove_all(dir);
}

TEST(SettingsStoreTest, EmptySelectedPathClearsExistingModelPath) {
  azookey::host::RuntimeSettings settings;
  settings.model.enabled = true;
  settings.model.selected_path.clear();

  azookey::host::EngineConfig config;
  config.model_path = "C:/models/existing.gguf";
  config.n_gpu_layers = 12;

  config = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, settings);

  EXPECT_TRUE(config.model_path.empty());
  EXPECT_FALSE(config.n_gpu_layers.has_value());
}

TEST(SettingsStoreTest, InvalidJsonIsQuarantinedAndDefaultsContinue) {
  const auto dir = TestDir("azookey_settings_invalid");
  const auto path = dir / "settings.json";
  WriteText(path, "{ invalid json");

  azookey::host::SettingsStore store(path);
  const auto result = store.Load();

  EXPECT_EQ(result.status, azookey::host::SettingsLoadStatus::Invalid);
  ASSERT_TRUE(result.error.has_value());
  ASSERT_TRUE(result.quarantined_path.has_value());
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_TRUE(std::filesystem::exists(*result.quarantined_path));
  EXPECT_FALSE(result.settings.live_conversion);
  EXPECT_TRUE(result.settings.prediction_enabled);

  std::filesystem::remove_all(dir);
}
