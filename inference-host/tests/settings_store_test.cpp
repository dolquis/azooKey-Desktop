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
  EXPECT_FALSE(result.settings.number_rewriter);
  EXPECT_FALSE(result.settings.katakana_rewriter);
  EXPECT_EQ(result.settings.nll, azookey::host::NllConfig{});
  EXPECT_EQ(result.settings.inference_threads, 0);
  EXPECT_EQ(result.settings.max_candidates, 9);
  EXPECT_EQ(result.settings.max_context_length, 10);
  EXPECT_TRUE(result.settings.prediction_enabled);
  EXPECT_EQ(result.settings.backend_preference, "auto");
  EXPECT_TRUE(result.settings.model.enabled);
  EXPECT_TRUE(result.settings.model.auto_load_on_host_start);

  std::filesystem::remove_all(dir);
}

TEST(SettingsStoreTest, NllSettingsClampAndReachEngineConfig) {
  const auto dir = TestDir("azookey_settings_nll");
  const auto path = dir / "settings.json";
  WriteText(path, R"({"reranker":{"nllRerankEnabled":true,"nllTopK":1e30,
      "nllWeight":-2,"nllBudgetMs":1,"nllFailureThreshold":100}})");
  azookey::host::SettingsStore store(path);
  const auto result = store.Load();
  ASSERT_EQ(result.status, azookey::host::SettingsLoadStatus::Loaded);
  const auto config = azookey::host::ApplyRuntimeSettingsToEngineConfig({}, result.settings);
  EXPECT_TRUE(config.nll.enabled);
  EXPECT_EQ(config.nll.top_k, 16);
  EXPECT_EQ(config.nll.weight, 0.0);
  EXPECT_EQ(config.nll.budget_ms, 5);
  EXPECT_EQ(config.nll.failure_threshold, 10);
  WriteText(path, R"({"reranker":{"nllRerankEnabled":"true","nllTopK":1.5,
      "nllWeight":"bad","nllBudgetMs":null,"nllFailureThreshold":false}})");
  EXPECT_EQ(store.Reload().settings.nll, azookey::host::NllConfig{});
  std::filesystem::remove_all(dir);
}

TEST(SettingsStoreTest, PartialFileFillsDefaultsAndAppliesEngineConfig) {
  const auto dir = TestDir("azookey_settings_partial");
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "liveConversion": true,
    "numberRewriter": true,
    "katakanaRewriter": true,
    "inferenceThreads": 6,
    "maxCandidates": 12,
    "maxContextLength": 20,
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
  EXPECT_TRUE(result.settings.number_rewriter);
  EXPECT_TRUE(result.settings.katakana_rewriter);
  EXPECT_EQ(result.settings.inference_threads, 6);
  EXPECT_EQ(result.settings.max_candidates, 12);
  EXPECT_EQ(result.settings.max_context_length, 20);
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
  ASSERT_TRUE(config.inference_threads.has_value());
  EXPECT_EQ(*config.inference_threads, 6);
  EXPECT_EQ(config.max_candidates, 12u);
  EXPECT_EQ(config.max_context_length, 20u);

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

TEST(SettingsStoreTest, VulkanPreferenceAndAutoResolveAgainstBuildDefault) {
  azookey::host::RuntimeSettings settings;
  azookey::host::EngineConfig config;
  settings.model.backend_preference = "vulkan";
  auto resolved = azookey::host::ApplyRuntimeSettingsToEngineConfig(
      config, settings, azookey::host::BackendKind::Cpu);
  EXPECT_EQ(resolved.backend, azookey::host::BackendKind::Vulkan);

  settings.model.backend_preference = "auto";
  resolved = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, settings,
                                                               azookey::host::BackendKind::Vulkan);
  EXPECT_EQ(resolved.backend, azookey::host::BackendKind::Vulkan);

  settings.model.backend_preference = "cpu";
  resolved = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, settings,
                                                               azookey::host::BackendKind::Vulkan);
  EXPECT_EQ(resolved.backend, azookey::host::BackendKind::Cpu);
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

TEST(SettingsStoreTest, NumericInferenceSettingsRejectWrongTypesAndOutOfRangeValues) {
  const auto dir = TestDir("azookey_settings_inference_numeric");
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "inferenceThreads": -5,
    "maxCandidates": 100,
    "maxContextLength": -1
  })");

  azookey::host::SettingsStore store(path);
  auto result = store.Load();
  EXPECT_EQ(result.settings.inference_threads, 0);
  EXPECT_EQ(result.settings.max_candidates, 9);
  EXPECT_EQ(result.settings.max_context_length, 10);

  WriteText(path, R"({
    "inferenceThreads": "8",
    "maxCandidates": 3.5,
    "maxContextLength": false
  })");
  result = store.Reload();
  EXPECT_EQ(result.settings.inference_threads, 0);
  EXPECT_EQ(result.settings.max_candidates, 9);
  EXPECT_EQ(result.settings.max_context_length, 10);

  std::filesystem::remove_all(dir);
}

TEST(SettingsStoreTest, AutomaticInferenceThreadsFollowPowerProfile) {
  using namespace azookey::host;
  for (const auto source : {PowerSource::Ac, PowerSource::Battery, PowerSource::Unknown}) {
    for (const unsigned int cpus : {0u, 1u, 2u, 3u, 4u, 7u, 8u, 16u}) {
      for (const std::string profile : {"auto", "performance", "battery_saver"}) {
        SCOPED_TRACE(profile + ": cpus=" + std::to_string(cpus));
        RuntimeSettings settings;
        settings.power_profile = profile;
        const auto provider = [=] { return InferenceThreadEnvironment{source, cpus}; };
        const auto config =
            ApplyRuntimeSettingsToEngineConfig({}, settings, BackendKind::Cpu, provider);
        const unsigned int target = profile == "performance"         ? 8u
                                    : profile == "battery_saver"     ? 2u
                                    : source == PowerSource::Ac      ? 8u
                                    : source == PowerSource::Battery ? 2u
                                                                     : 4u;
        const auto expected = cpus == 0 ? 1u : cpus < target ? cpus : target;
        EXPECT_EQ(config.inference_threads, static_cast<int32_t>(expected));
      }
    }
  }
}

TEST(SettingsStoreTest, ExplicitInferenceThreadsOverrideEnvironmentWithoutQueryingIt) {
  using namespace azookey::host;
  for (const std::string profile : {"auto", "performance", "battery_saver"}) {
    for (int32_t threads = 1; threads <= 8; ++threads) {
      RuntimeSettings settings;
      settings.power_profile = profile;
      settings.inference_threads = threads;
      const auto config = ApplyRuntimeSettingsToEngineConfig({}, settings, BackendKind::Cpu, [] {
        ADD_FAILURE() << "explicit thread count must not query the environment";
        return InferenceThreadEnvironment{PowerSource::Battery, 1};
      });
      EXPECT_EQ(config.inference_threads, threads);
    }
  }
}

TEST(SettingsStoreTest, ReloadResamplesPowerSourceAndHonorsNewExplicitThreads) {
  using namespace azookey::host;
  const auto dir = TestDir("azookey_settings_power_reload");
  const auto path = dir / "settings.json";
  WriteText(path, R"({"powerProfile":"auto","inferenceThreads":0})");
  SettingsStore store(path);
  PowerSource source = PowerSource::Ac;
  const auto provider = [&] { return InferenceThreadEnvironment{source, 6}; };
  const auto loaded = store.Load();
  ASSERT_EQ(loaded.status, SettingsLoadStatus::Loaded);
  auto config = ApplyRuntimeSettingsToEngineConfig({}, loaded.settings, BackendKind::Cpu, provider);
  EXPECT_EQ(config.inference_threads, 6);
  source = PowerSource::Battery;
  const auto reloaded = store.Reload();
  ASSERT_EQ(reloaded.status, SettingsLoadStatus::Loaded);
  config =
      ApplyRuntimeSettingsToEngineConfig(config, reloaded.settings, BackendKind::Cpu, provider);
  EXPECT_EQ(config.inference_threads, 2);
  WriteText(path, R"({"powerProfile":"battery_saver","inferenceThreads":8})");
  const auto explicit_settings = store.Reload();
  ASSERT_EQ(explicit_settings.status, SettingsLoadStatus::Loaded);
  config = ApplyRuntimeSettingsToEngineConfig(config, explicit_settings.settings, BackendKind::Cpu,
                                              provider);
  EXPECT_EQ(config.inference_threads, 8);
  std::filesystem::remove_all(dir);
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
