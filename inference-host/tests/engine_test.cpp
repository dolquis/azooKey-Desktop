#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
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
    azookey::learning::LearningStore& store,
    azookey::host::EngineConfig cfg) {
  return std::make_unique<azookey::host::InferenceEngine>(
      std::make_unique<azookey::core::SimpleConverter>(), &store, cfg);
}

std::unique_ptr<azookey::host::InferenceEngine> MakeEngine(
    azookey::learning::LearningStore& store) {
  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  return MakeEngine(store, cfg);
}

std::string TempPath(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}
bool WaitForFileExists(const std::string& path, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::filesystem::exists(path)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return std::filesystem::exists(path);
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

class BlockingConverter final : public azookey::core::IConverter {
 public:
  std::vector<azookey::core::Candidate> Convert(
      const std::string& kana,
      const azookey::core::ConversionContext&) override {
    WaitForRelease();
    return {azookey::core::Candidate{kana, kana, 1.0,
                                     azookey::core::CandidateSource::Heuristic,
                                     "blocking-converter"}};
  }

  std::vector<azookey::core::Candidate> PredictNext(
      const std::string& kana,
      const azookey::core::ConversionContext& context) override {
    return Convert(kana, context);
  }

  std::vector<azookey::core::Candidate> Correct(
      const std::string& kana,
      const azookey::core::CorrectionHint&,
      const azookey::core::ConversionContext& context) override {
    return Convert(kana, context);
  }

  void Commit(const azookey::core::Candidate&, const azookey::core::ConversionContext&) override {}
  void Learn(const std::string&, const std::string&) override {}

  bool WaitUntilEntered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this]() { return entered_; });
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }

 private:
  void WaitForRelease() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this]() { return released_; });
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  bool entered_{false};
  bool released_{false};
};
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

  engine.reset();
  std::remove(path);
}

TEST(InferenceEngineTest, CommitObservationDebouncesUntilCountThreshold) {
  const std::string path = TempPath("azookey_host_engine_learning_debounce_count.tsv");
  std::remove(path.c_str());
  azookey::learning::LearningStore store(path);

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 3;
  cfg.learning_flush_interval_sec = 1000;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  engine->CommitObservation("reading", "surface", kNowBase + 1);
  engine->CommitObservation("reading", "surface", kNowBase + 2);
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_TRUE(store.dirty());

  engine->CommitObservation("reading", "surface", kNowBase + 3);
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(store.dirty());

  engine.reset();
  std::remove(path.c_str());
}

TEST(InferenceEngineTest, CommitObservationFlushesAfterInterval) {
  const std::string path = TempPath("azookey_host_engine_learning_debounce_interval.tsv");
  std::remove(path.c_str());
  azookey::learning::LearningStore store(path);

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 100;
  cfg.learning_flush_interval_sec = 5;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  engine->CommitObservation("reading", "surface", kNowBase + 10);
  engine->CommitObservation("reading", "surface", kNowBase + 14);
  EXPECT_FALSE(std::filesystem::exists(path));

  engine->CommitObservation("reading", "surface", kNowBase + 15);
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(store.dirty());

  engine.reset();
  std::remove(path.c_str());
}

TEST(InferenceEngineTest, CommitObservationFlushesAfterIntervalWithoutAnotherObservation) {
  const std::string path = TempPath("azookey_host_engine_learning_idle_interval.tsv");
  std::remove(path.c_str());
  azookey::learning::LearningStore store(path);

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 100;
  cfg.learning_flush_interval_sec = 1;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  engine->CommitObservation("reading", "surface", kNowBase + 10);
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_TRUE(WaitForFileExists(path, std::chrono::milliseconds(2500)));

  azookey::learning::LearningStore loaded(path);
  ASSERT_TRUE(loaded.Load());
  EXPECT_GT(loaded.Score("reading", "surface", kNowBase + 10), 0.0);

  engine.reset();
  std::remove(path.c_str());
}

TEST(InferenceEngineTest, FlushLearningStorePersistsPendingObservation) {
  const std::string path = TempPath("azookey_host_engine_learning_flush.tsv");
  std::remove(path.c_str());
  azookey::learning::LearningStore store(path);

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 100;
  cfg.learning_flush_interval_sec = 1000;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  engine->CommitObservation("reading", "surface", kNowBase + 1);
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_TRUE(engine->FlushLearningStore());
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(store.dirty());

  engine.reset();
  std::remove(path.c_str());
}

TEST(InferenceEngineTest, SaveFailureKeepsDirtyStateAndCanRetry) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_host_engine_learning_save_failure";
  const auto blocked_path = root / "learning-as-directory.tsv";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(blocked_path);
  azookey::learning::LearningStore store(blocked_path.string());

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 1;
  cfg.learning_flush_interval_sec = 1000;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  engine->CommitObservation("reading", "surface", kNowBase + 1);
  EXPECT_TRUE(store.dirty());
  ASSERT_TRUE(engine->last_error().has_value());
  EXPECT_NE(engine->last_error()->find("failed to save learning store"), std::string::npos);

  std::filesystem::remove_all(root);
  EXPECT_TRUE(engine->FlushLearningStore());
  EXPECT_FALSE(store.dirty());
  EXPECT_TRUE(std::filesystem::exists(blocked_path));

  engine.reset();
  std::filesystem::remove_all(root);
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
  EXPECT_FALSE(engine->last_error().has_value());

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, LoadModelFailureKeepsPreviouslyLoadedModel) {
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
  EXPECT_TRUE(engine->model_loaded());
  EXPECT_FALSE(engine->last_error().has_value());

  auto cands = engine->QueryCandidates("にほん", "", kNowBase);
  ASSERT_FALSE(cands.empty());
  EXPECT_NE(cands.front().debug_info.find("zenzai-gguf-loaded"),
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

TEST(InferenceEngineTest, QueryCandidatesSerializesConcurrentLoadModel) {
  using namespace std::chrono_literals;

  const char* lpath = "azookey_host_engine_query_load_serialized.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);

  auto converter = std::make_unique<BlockingConverter>();
  auto* blocking_converter = converter.get();
  azookey::host::EngineConfig cfg;
  azookey::host::InferenceEngine engine(std::move(converter), &store, cfg);

  const std::string model_path = TempPath("azookey_query_load_serialized.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  std::vector<azookey::core::Candidate> query_result;
  std::thread query_thread([&]() {
    query_result = engine.QueryCandidates("にほん", "", kNowBase);
  });

  const bool query_entered = blocking_converter->WaitUntilEntered(1s);
  EXPECT_TRUE(query_entered);

  std::promise<void> load_started_promise;
  auto load_started = load_started_promise.get_future();
  auto load_future = std::async(std::launch::async, [&]() {
    load_started_promise.set_value();
    azookey::host::ModelLoadOptions options;
    options.path = model_path;
    return engine.LoadModelWithResult(options);
  });

  const bool load_started_ready = load_started.wait_for(1s) == std::future_status::ready;
  EXPECT_TRUE(load_started_ready);
  if (query_entered && load_started_ready) {
    EXPECT_EQ(load_future.wait_for(50ms), std::future_status::timeout);
  }

  blocking_converter->Release();
  query_thread.join();

  const auto load_result = load_future.get();
  EXPECT_TRUE(load_result.ok);
  EXPECT_TRUE(engine.model_loaded());
  ASSERT_FALSE(query_result.empty());
  EXPECT_EQ(query_result.front().debug_info, "blocking-converter");

  std::remove(model_path.c_str());
  std::remove(lpath);
}
