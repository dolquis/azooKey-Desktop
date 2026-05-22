#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/UserDictionary.h"

namespace {

constexpr uint64_t kNowBase = 1'700'000'000ULL;

std::unique_ptr<azookey::host::InferenceEngine> MakeEngine(
    azookey::learning::LearningStore& store) {
  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  return std::make_unique<azookey::host::InferenceEngine>(
      std::make_unique<azookey::core::SimpleConverter>(), &store, cfg);
}

std::string TempPath(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

void WriteMinimalGguf(const std::string& path, uint32_t version = 3) {
  std::ofstream out(path, std::ios::binary);
  out.write("GGUF", 4);
  const unsigned char bytes[4] = {
      static_cast<unsigned char>(version & 0xFF),
      static_cast<unsigned char>((version >> 8) & 0xFF),
      static_cast<unsigned char>((version >> 16) & 0xFF),
      static_cast<unsigned char>((version >> 24) & 0xFF),
  };
  out.write(reinterpret_cast<const char*>(bytes), 4);
}

}  // namespace

TEST(InferenceEngineTest, QueryWithLearningBoost) {
  const char* path = "azookey_host_engine_learning.tsv";
  std::remove(path);
  azookey::learning::LearningStore store(path);

  auto engine = MakeEngine(store);

  // First conversion - 日本 is the static top.
  auto first = engine->QueryCandidates("にほん", "", kNowBase);
  ASSERT_FALSE(first.empty());
  EXPECT_EQ(first.front().surface, "日本");

  // Commit 二本 three times to outweigh the static gap.
  engine->CommitObservation("にほん", "二本", kNowBase + 1);
  engine->CommitObservation("にほん", "二本", kNowBase + 2);
  engine->CommitObservation("にほん", "二本", kNowBase + 3);

  auto fourth = engine->QueryCandidates("にほん", "", kNowBase + 4);
  ASSERT_FALSE(fourth.empty());
  EXPECT_EQ(fourth.front().surface, "二本");

  std::remove(path);
}

TEST(InferenceEngineTest, UserDictionaryInjection) {
  const char* lpath = "azookey_host_engine_user_dict_learn.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string udict_path =
      (std::filesystem::temp_directory_path() / "azookey_host_engine_user.json").string();
  azookey::learning::UserDictionary dict(udict_path);
  azookey::learning::UserWord w;
  w.word = "azooKey";
  w.ruby = "あずきい";
  dict.Add(w);
  engine->SetUserDictionary(&dict);

  auto cands = engine->QueryCandidates("あずきい", "", kNowBase);
  ASSERT_FALSE(cands.empty());
  EXPECT_EQ(cands.front().surface, "azooKey");
  EXPECT_NE(cands.front().debug_info.find("user-dict"), std::string::npos);

  // Removing the user word makes it disappear from results.
  ASSERT_TRUE(dict.Remove("azooKey", "あずきい"));
  auto cands2 = engine->QueryCandidates("あずきい", "", kNowBase);
  for (const auto& c : cands2) {
    EXPECT_NE(c.surface, "azooKey");
  }

  std::remove(lpath);
}

TEST(InferenceEngineTest, CancelEarlyReturn) {
  const char* lpath = "azookey_host_engine_cancel.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  std::atomic<bool> cancel{true};
  auto cands = engine->QueryCandidates("にほん", "", kNowBase, &cancel);
  EXPECT_TRUE(cands.empty());

  cancel.store(false);
  auto cands2 = engine->QueryCandidates("にほん", "", kNowBase, &cancel);
  EXPECT_FALSE(cands2.empty());

  std::remove(lpath);
}

TEST(InferenceEngineTest, LegacyOverloadStillWorks) {
  const char* lpath = "azookey_host_engine_legacy.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  // Three-argument overload exists for backwards compatibility with main.cpp
  // and the existing bench harness.
  auto cands = engine->QueryCandidates("わたし", "", kNowBase);
  EXPECT_FALSE(cands.empty());
  std::remove(lpath);
}

TEST(InferenceEngineTest, LoadModelFallbackWithoutPath) {
  const char* lpath = "azookey_host_engine_load_empty.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  EXPECT_TRUE(engine->LoadModel());
  EXPECT_FALSE(engine->model_loaded());
  EXPECT_FALSE(engine->last_error().has_value());

  std::remove(lpath);
}

TEST(InferenceEngineTest, LoadModelRecordsOptionsAndMissingPath) {
  const char* lpath = "azookey_host_engine_load_missing.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  azookey::host::ModelLoadOptions options;
  options.path = "azookey_missing_zenzai_model.gguf";
  options.backend = azookey::host::BackendKind::Cuda;
  options.n_gpu_layers = 35;

  const auto result = engine->LoadModelWithResult(options);
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error, engine->last_error());
  EXPECT_EQ(engine->backend(), azookey::host::BackendKind::Cuda);
  EXPECT_EQ(engine->config().model_path, options.path);
  ASSERT_TRUE(engine->config().n_gpu_layers.has_value());
  EXPECT_EQ(engine->config().n_gpu_layers.value(), 35);
  EXPECT_FALSE(engine->model_loaded());

  std::remove(lpath);
}

TEST(InferenceEngineTest, LoadModelLoadsValidGgufWithCpuBackend) {
  const char* lpath = "azookey_host_engine_load_valid.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_minimal_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  options.backend = azookey::host::BackendKind::Cpu;

  const auto result = engine->LoadModelWithResult(options);
  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.error.has_value());
  EXPECT_TRUE(engine->model_loaded());
  EXPECT_FALSE(engine->last_error().has_value());

  auto cands = engine->QueryCandidates("にほん", "", kNowBase);
  ASSERT_FALSE(cands.empty());
  EXPECT_NE(cands.front().debug_info.find("zenzai-gguf-loaded"),
            std::string::npos);

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, LoadModelRejectsInvalidGguf) {
  const char* lpath = "azookey_host_engine_load_invalid.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_invalid_zenzai.gguf");
  {
    std::ofstream out(model_path, std::ios::binary);
    out << "not gguf";
  }

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  const auto result = engine->LoadModelWithResult(options);
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.error.has_value());
  EXPECT_FALSE(engine->model_loaded());

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, LoadModelCudaFallsBackToCpuForNow) {
  const char* lpath = "azookey_host_engine_load_cuda.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_cuda_fallback_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  options.backend = azookey::host::BackendKind::Cuda;
  options.n_gpu_layers = 35;

  const auto result = engine->LoadModelWithResult(options);
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.error.has_value());
  EXPECT_EQ(engine->backend(), azookey::host::BackendKind::Cpu);
  EXPECT_TRUE(engine->model_loaded());
  EXPECT_EQ(engine->last_error(), result.error);

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, LoadModelFailureRestoresFallbackConverter) {
  const char* lpath = "azookey_host_engine_reload_failure.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_reload_success_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions good;
  good.path = model_path;
  ASSERT_TRUE(engine->LoadModelWithResult(good).ok);
  ASSERT_TRUE(engine->model_loaded());

  azookey::host::ModelLoadOptions bad;
  bad.path = TempPath("azookey_missing_after_success.gguf");
  const auto failed = engine->LoadModelWithResult(bad);
  EXPECT_FALSE(failed.ok);
  EXPECT_FALSE(engine->model_loaded());

  auto cands = engine->QueryCandidates("にほん", "", kNowBase);
  ASSERT_FALSE(cands.empty());
  EXPECT_EQ(cands.front().debug_info.find("zenzai-gguf-loaded"),
            std::string::npos);

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, LoadModelSuccessDoesNotChainWrappers) {
  const char* lpath = "azookey_host_engine_reload_success.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_reload_chain_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);

  auto cands = engine->QueryCandidates("にほん", "", kNowBase);
  ASSERT_FALSE(cands.empty());
  const auto& debug = cands.front().debug_info;
  const auto first = debug.find("zenzai-gguf-loaded");
  ASSERT_NE(first, std::string::npos);
  EXPECT_EQ(debug.find("zenzai-gguf-loaded", first + 1), std::string::npos);

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, LoadModelStateAccessorsThreadedSmoke) {
  const char* lpath = "azookey_host_engine_load_threaded.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  std::thread writer([&engine]() {
    for (int i = 0; i < 100; ++i) {
      azookey::host::ModelLoadOptions options;
      options.path = "azookey_missing_zenzai_model_threaded.gguf";
      options.backend = (i % 2 == 0) ? azookey::host::BackendKind::Cpu
                                     : azookey::host::BackendKind::Cuda;
      options.n_gpu_layers = i;
      engine->LoadModel(options);
    }
  });

  std::thread reader([&engine]() {
    for (int i = 0; i < 100; ++i) {
      (void)engine->backend();
      (void)engine->config();
      (void)engine->model_loaded();
      (void)engine->last_error();
    }
  });

  writer.join();
  reader.join();
  EXPECT_FALSE(engine->model_loaded());

  std::remove(lpath);
}
