#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "azookey/host/SettingsStore.h"
#include "azookey/learning/AtomicFile.h"
#include "azookey/learning/FileLock.h"

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

TEST(SettingsStoreTest, AutoBackendCanUseExplicitDefaultBackend) {
  azookey::host::RuntimeSettings settings;
  settings.backend_preference = "auto";

  azookey::host::EngineConfig config;
  config.backend = azookey::host::BackendKind::Cuda;

  auto legacy_fallback = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, settings);
  EXPECT_EQ(legacy_fallback.backend, azookey::host::BackendKind::Cuda);

  auto explicit_default = azookey::host::ApplyRuntimeSettingsToEngineConfig(
      config, settings, azookey::host::BackendKind::Cpu);
  EXPECT_EQ(explicit_default.backend, azookey::host::BackendKind::Cpu);
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

TEST(SettingsStoreTest, ReadFailureDoesNotQuarantineFile) {
  const auto dir = TestDir("azookey_settings_read_failure");
  const auto path = dir / "settings.json";
  WriteText(path, R"({"liveConversion":true})");

#ifdef _WIN32
  HANDLE exclusive = CreateFileW(path.wstring().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(exclusive, INVALID_HANDLE_VALUE);
#else
  ASSERT_EQ(chmod(path.c_str(), 0), 0);
#endif

  azookey::host::SettingsStore store(path, std::chrono::milliseconds(20));
  const auto result = store.Load();

#ifdef _WIN32
  CloseHandle(exclusive);
#else
  ASSERT_EQ(chmod(path.c_str(), S_IRUSR | S_IWUSR), 0);
#endif

  EXPECT_EQ(result.status, azookey::host::SettingsLoadStatus::Invalid);
  EXPECT_FALSE(result.quarantined_path.has_value());
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(path.string() + ".invalid"));
  std::filesystem::remove_all(dir);
}

TEST(SettingsStoreTest, LockTimeoutLeavesInvalidFileForLaterQuarantine) {
  const auto dir = TestDir("azookey_settings_lock_timeout");
  const auto path = dir / "settings.json";
  WriteText(path, "{ invalid json");

  std::promise<bool> acquired;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::thread holder([&] {
    auto lock =
        azookey::learning::AcquireExclusiveFileLockForPath(path, std::chrono::milliseconds(1000));
    acquired.set_value(lock.has_value());
    if (lock) release_future.wait();
  });
  const bool has_lock = acquired.get_future().get();
  if (!has_lock) {
    release.set_value();
    holder.join();
  }
  ASSERT_TRUE(has_lock);

  azookey::host::SettingsStore store(path, std::chrono::milliseconds(20));
  const auto blocked_result = store.Load();
  EXPECT_EQ(blocked_result.status, azookey::host::SettingsLoadStatus::Invalid);
  EXPECT_FALSE(blocked_result.quarantined_path.has_value());
  EXPECT_TRUE(std::filesystem::exists(path));

  release.set_value();
  holder.join();

  const auto retry_result = store.Load();
  EXPECT_EQ(retry_result.status, azookey::host::SettingsLoadStatus::Invalid);
  EXPECT_TRUE(retry_result.quarantined_path.has_value());
  EXPECT_FALSE(std::filesystem::exists(path));
  std::filesystem::remove_all(dir);
}

TEST(SettingsStoreTest, SharedLockSerializesAtomicWriterBeforeRead) {
  const auto dir = TestDir("azookey_settings_serialized_writer");
  const auto path = dir / "settings.json";
  WriteText(path, "{ invalid json");

  std::promise<bool> acquired;
  bool write_succeeded = false;
  std::thread writer([&] {
    auto lock =
        azookey::learning::AcquireExclusiveFileLockForPath(path, std::chrono::milliseconds(1000));
    acquired.set_value(lock.has_value());
    if (!lock) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write_succeeded = azookey::learning::WriteTextFileAtomically(
        path, R"({"liveConversion":true,"predictionEnabled":true})");
  });
  const bool has_lock = acquired.get_future().get();
  if (!has_lock) writer.join();
  ASSERT_TRUE(has_lock);

  azookey::host::SettingsStore store(path, std::chrono::milliseconds(1000));
  const auto result = store.Load();
  writer.join();

  EXPECT_TRUE(write_succeeded);
  EXPECT_EQ(result.status, azookey::host::SettingsLoadStatus::Loaded);
  EXPECT_TRUE(result.settings.live_conversion);
  EXPECT_FALSE(result.quarantined_path.has_value());
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(path.string() + ".invalid"));
  std::filesystem::remove_all(dir);
}

TEST(SettingsStoreTest, InvalidReloadKeepsCurrentSettings) {
  const auto dir = TestDir("azookey_settings_reload_invalid");
  const auto path = dir / "settings.json";
  WriteText(path, R"({"liveConversion":true,"logLevel":"debug"})");

  azookey::host::SettingsStore store(path);
  ASSERT_EQ(store.Load().status, azookey::host::SettingsLoadStatus::Loaded);
  ASSERT_TRUE(store.settings().live_conversion);
  ASSERT_EQ(store.settings().log_level, "debug");

  WriteText(path, "{ invalid json");
  const auto result = store.Reload();

  EXPECT_EQ(result.status, azookey::host::SettingsLoadStatus::Invalid);
  EXPECT_TRUE(result.settings.live_conversion);
  EXPECT_EQ(result.settings.log_level, "debug");
  EXPECT_TRUE(store.settings().live_conversion);
  EXPECT_EQ(store.settings().log_level, "debug");
  EXPECT_EQ(store.last_result().settings.log_level, "debug");
  std::filesystem::remove_all(dir);
}
