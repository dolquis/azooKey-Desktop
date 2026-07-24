#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/ZenzaiModelConverter.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/UserDictionary.h"

namespace {

constexpr uint64_t kNowBase = 1'700'000'000ULL;

std::unique_ptr<azookey::host::InferenceEngine> MakeEngine(azookey::learning::LearningStore& store,
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

void EnableMockZenzaiCandidatesForTests(azookey::host::ModelLoadOptions& options) {
  options.mock_zenzai_candidates_for_tests = true;
}

bool ProbeOnlyGgufUnsupportedWithRealLlama() {
#if AZOOKEY_WITH_LLAMA_CPP
  return true;
#else
  return false;
#endif
}

size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

class BlockingConverter final : public azookey::core::IConverter {
 public:
  std::vector<azookey::core::Candidate> Convert(const std::string& kana,
                                                const azookey::core::ConversionContext&) override {
    WaitForRelease();
    return {azookey::core::Candidate{kana, kana, 1.0, azookey::core::CandidateSource::Heuristic,
                                     "blocking-converter"}};
  }

  std::vector<azookey::core::Candidate> PredictNext(
      const std::string& kana, const azookey::core::ConversionContext& context) override {
    return Convert(kana, context);
  }

  std::vector<azookey::core::Candidate> Correct(
      const std::string& kana, const azookey::core::CorrectionHint&,
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

class ThrowingLearningStore final : public azookey::learning::LearningStore {
 public:
  ThrowingLearningStore()
      : azookey::learning::LearningStore(TempPath("azookey_host_engine_reranker_throw.tsv")) {}

  double Score(const std::string&, const std::string&, uint64_t) const override {
    throw std::runtime_error("score failure");
  }
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

TEST(InferenceEngineTest, PrunesLearningStoreOnlyAtFlushBoundary) {
  const std::string path = TempPath("azookey_host_engine_learning_prune_on_flush.tsv");
  std::remove(path.c_str());
  azookey::learning::LearningStore store(path);

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 100;
  cfg.learning_flush_interval_sec = 1000;
  cfg.learning_max_records = 1;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  engine->CommitObservation("a", "first", kNowBase + 1);
  engine->CommitObservation("b", "second", kNowBase + 2);
  engine->CommitObservation("b", "second", kNowBase + 3);
  EXPECT_EQ(store.size(), 2u);
  EXPECT_FALSE(std::filesystem::exists(path));

  EXPECT_TRUE(engine->FlushLearningStore());
  EXPECT_EQ(store.size(), 1u);
  EXPECT_EQ(store.Score("a", "first", kNowBase + 3), 0.0);
  EXPECT_GT(store.Score("b", "second", kNowBase + 3), 0.0);
  EXPECT_TRUE(std::filesystem::exists(path));

  engine.reset();
  std::remove(path.c_str());
}

TEST(InferenceEngineTest, SaveFailureKeepsDirtyStateAndCanRetry) {
  const auto root =
      std::filesystem::temp_directory_path() / "azookey_host_engine_learning_save_failure";
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
  EXPECT_EQ(cands.front().source, azookey::core::CandidateSource::UserDictionary);
  EXPECT_NE(cands.front().debug_info.find("user-dict"), std::string::npos);

  // Removing the user word makes it disappear from results.
  ASSERT_TRUE(dict.Remove("azooKey", "あずきい"));
  auto cands2 = engine->QueryCandidates("あずきい", "", kNowBase);
  for (const auto& c : cands2) {
    EXPECT_NE(c.surface, "azooKey");
  }

  std::remove(lpath);
}

TEST(InferenceEngineTest, AddUserWordReloadsDiskBeforeSaving) {
  const std::string lpath = TempPath("azookey_host_engine_user_dict_reload_add.tsv");
  const std::string udict_path = TempPath("azookey_host_engine_user_dict_reload_add.json");
  std::remove(lpath.c_str());
  std::remove(udict_path.c_str());

  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);
  azookey::learning::UserDictionary dict(udict_path);
  ASSERT_TRUE(dict.Load());
  engine->SetUserDictionary(&dict);

  azookey::learning::UserWord external;
  external.word = "External";
  external.ruby = "external";
  {
    azookey::learning::UserDictionary writer(udict_path);
    ASSERT_TRUE(writer.Load());
    writer.Add(external);
    ASSERT_TRUE(writer.Save());
  }

  azookey::learning::UserWord added;
  added.word = "Added";
  added.ruby = "added";
  ASSERT_TRUE(engine->AddUserWord(added));

  azookey::learning::UserDictionary loaded(udict_path);
  ASSERT_TRUE(loaded.Load());
  EXPECT_EQ(loaded.Lookup("external").size(), 1u);
  EXPECT_EQ(loaded.Lookup("added").size(), 1u);

  std::remove(udict_path.c_str());
  std::remove(lpath.c_str());
}

TEST(InferenceEngineTest, RemoveUserWordReloadsDiskBeforeSaving) {
  const std::string lpath = TempPath("azookey_host_engine_user_dict_reload_remove.tsv");
  const std::string udict_path = TempPath("azookey_host_engine_user_dict_reload_remove.json");
  std::remove(lpath.c_str());
  std::remove(udict_path.c_str());

  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);
  azookey::learning::UserDictionary dict(udict_path);
  ASSERT_TRUE(dict.Load());
  engine->SetUserDictionary(&dict);

  azookey::learning::UserWord external;
  external.word = "External";
  external.ruby = "external";
  {
    azookey::learning::UserDictionary writer(udict_path);
    ASSERT_TRUE(writer.Load());
    writer.Add(external);
    ASSERT_TRUE(writer.Save());
  }

  ASSERT_TRUE(engine->RemoveUserWord("External", "external"));

  azookey::learning::UserDictionary loaded(udict_path);
  ASSERT_TRUE(loaded.Load());
  EXPECT_TRUE(loaded.Lookup("external").empty());

  std::remove(udict_path.c_str());
  std::remove(lpath.c_str());
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

TEST(InferenceEngineTest, RerankerFailureFallsBackToRawCandidates) {
  ThrowingLearningStore store;
  auto engine = MakeEngine(store);

  auto candidates = engine->QueryCandidates("にほん", "", kNowBase);
  ASSERT_FALSE(candidates.empty());
  EXPECT_EQ(candidates.front().surface, "日本");
  ASSERT_TRUE(engine->last_error().has_value());
  ASSERT_TRUE(engine->effective_last_error().has_value());
  EXPECT_NE(engine->last_error()->find("reranker failed: score failure"), std::string::npos);

  auto predictions = engine->QueryPredictions("にほん", "", kNowBase);
  ASSERT_FALSE(predictions.empty());
  EXPECT_NE(predictions.front().debug_info.find("predict"), std::string::npos);

  auto corrections = engine->QueryCorrections("にほん", "", "日本", kNowBase);
  ASSERT_FALSE(corrections.empty());
  EXPECT_NE(corrections.front().surface, "日本");
  ASSERT_TRUE(engine->last_error().has_value());
  EXPECT_NE(engine->last_error()->find("reranker failed"), std::string::npos);
}

TEST(InferenceEngineTest, LoadModelFallbackWithoutPath) {
  const char* lpath = "azookey_host_engine_load_empty.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const auto result = engine->LoadModelWithResult();
  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.error.has_value());
  EXPECT_FALSE(engine->model_loaded());
  EXPECT_FALSE(engine->last_error().has_value());
  EXPECT_TRUE(engine->config().model_path.empty());

  auto cands = engine->QueryCandidates("にほん", "", kNowBase);
  ASSERT_FALSE(cands.empty());
  EXPECT_EQ(cands.front().surface, "日本");

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
  ASSERT_TRUE(result.error.has_value());
  EXPECT_NE(result.error->find("model file"), std::string::npos);
  EXPECT_EQ(result.error, engine->last_error());
  EXPECT_EQ(engine->backend(), azookey::host::BackendKind::Cuda);
  EXPECT_EQ(engine->config().model_path, options.path);
  ASSERT_TRUE(engine->config().n_gpu_layers.has_value());
  EXPECT_EQ(engine->config().n_gpu_layers.value(), 35);
  EXPECT_FALSE(engine->model_loaded());

  auto cands = engine->QueryCandidates("にほん", "", kNowBase);
  ASSERT_FALSE(cands.empty());
  EXPECT_EQ(cands.front().surface, "日本");

  std::remove(lpath);
}

TEST(InferenceEngineTest, StartModelPreloadKeepsFallbackResponsiveWhileLoadIsPending) {
  using namespace std::chrono_literals;

  const char* lpath = "azookey_host_engine_preload_pending.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  std::promise<void> probe_entered_promise;
  auto probe_entered = probe_entered_promise.get_future();
  std::promise<void> release_probe_promise;
  auto release_probe = release_probe_promise.get_future();

  azookey::host::ModelLoadOptions options;
  options.path = "azookey_missing_preload_pending_zenzai.gguf";
  options.backend = azookey::host::BackendKind::Cpu;
  options.before_probe_for_tests = [&]() {
    probe_entered_promise.set_value();
    (void)release_probe.wait_for(1s);
  };

  EXPECT_TRUE(engine->StartModelPreload(options));
  EXPECT_TRUE(engine->model_preload_in_progress());
  const bool preload_entered = probe_entered.wait_for(1s) == std::future_status::ready;
  EXPECT_TRUE(preload_entered);
  const auto preload_health = engine->health_snapshot();
  EXPECT_TRUE(preload_health.model_preload_in_progress);
  EXPECT_FALSE(preload_health.model_loaded);
  EXPECT_FALSE(preload_health.last_error.has_value());

  auto next_config = engine->config();
  next_config.learning_alpha = 0.42;
  engine->ApplyConfig(next_config);

  auto query_future = std::async(
      std::launch::async, [&engine]() { return engine->QueryCandidates("にほん", "", kNowBase); });
  EXPECT_EQ(query_future.wait_for(100ms), std::future_status::ready);
  const auto cands = query_future.get();
  ASSERT_FALSE(cands.empty());
  EXPECT_EQ(cands.front().surface, "日本");

  release_probe_promise.set_value();
  engine->WaitForModelPreload();
  EXPECT_FALSE(engine->model_preload_in_progress());
  EXPECT_FALSE(engine->model_loaded());
  EXPECT_DOUBLE_EQ(engine->config().learning_alpha, 0.42);
  ASSERT_TRUE(engine->last_error().has_value());
  EXPECT_NE(engine->last_error()->find("model file"), std::string::npos);
  const auto failed_health = engine->health_snapshot();
  EXPECT_FALSE(failed_health.model_preload_in_progress);
  EXPECT_FALSE(failed_health.model_loaded);
  EXPECT_EQ(failed_health.model_path, options.path);
  ASSERT_TRUE(failed_health.last_error.has_value());
  EXPECT_NE(failed_health.last_error->find("model file"), std::string::npos);

  std::remove(lpath);
}

TEST(InferenceEngineTest, ProbeZenzaiGgufModelRejectsMissingPath) {
  const std::string model_path = TempPath("azookey_probe_missing_zenzai.gguf");
  std::remove(model_path.c_str());

  const auto result = azookey::host::ProbeZenzaiGgufModel(model_path);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.info.path, model_path);
  EXPECT_NE(result.error.find("model file"), std::string::npos);
  EXPECT_FALSE(result.runtime);
}

TEST(InferenceEngineTest, ProbeZenzaiGgufModelAcceptsMinimalHeader) {
  const std::string model_path = TempPath("azookey_probe_minimal_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  const auto result = azookey::host::ProbeZenzaiGgufModel(model_path);
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.info.path, model_path);
  EXPECT_EQ(result.info.file_size_bytes, 8u);
  EXPECT_EQ(result.info.gguf_version, 3u);
  EXPECT_TRUE(result.error.empty());
  EXPECT_FALSE(result.runtime);

  std::remove(model_path.c_str());
}

TEST(InferenceEngineTest, ProbeZenzaiGgufModelClassifiesUnsupportedVersion) {
  const std::string model_path = TempPath("azookey_probe_unsupported_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path, 99);

  const auto result = azookey::host::ProbeZenzaiGgufModel(model_path);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.info.path, model_path);
  EXPECT_EQ(result.info.file_size_bytes, 8u);
  EXPECT_EQ(result.info.gguf_version, 99u);
  EXPECT_EQ(result.error, "unsupported GGUF version: 99");
  EXPECT_FALSE(result.runtime);

  std::remove(model_path.c_str());
}

#if AZOOKEY_WITH_LLAMA_CPP
TEST(InferenceEngineTest, RealLlamaLoadFailureSurfacesDetailedDiagnostic) {
  const char* lpath = "azookey_host_engine_load_diagnostic.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_invalid_llama_model.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  options.backend = azookey::host::BackendKind::Cpu;
  const auto result = engine->LoadModelWithResult(options);

  std::remove(model_path.c_str());
  std::remove(lpath);

  EXPECT_FALSE(result.ok);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_NE(result.error->find("llama.cpp model load failed:"), std::string::npos);
  EXPECT_GT(result.error->size(), std::string("llama.cpp model load failed:").size());
  const auto health = engine->health_snapshot();
  ASSERT_TRUE(health.last_error.has_value());
  EXPECT_EQ(health.last_error, result.error);
}
#endif

TEST(InferenceEngineTest, ResolvesOnlyZenzaiCustomPreTokenizerToUpstreamGpt2) {
  const auto resolved =
      azookey::host::ResolveZenzaiPreTokenizerOverride("gpt2-small-japanese-char");
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(*resolved, "gpt-2");

  EXPECT_FALSE(azookey::host::ResolveZenzaiPreTokenizerOverride("gpt-2").has_value());
  EXPECT_FALSE(azookey::host::ResolveZenzaiPreTokenizerOverride("default").has_value());
  EXPECT_FALSE(azookey::host::ResolveZenzaiPreTokenizerOverride("").has_value());
}

TEST(InferenceEngineTest, LoadModelLoadsValidGgufWithCpuBackend) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

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
  EnableMockZenzaiCandidatesForTests(options);

  const auto result = engine->LoadModelWithResult(options);
  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.error.has_value());
  EXPECT_TRUE(engine->model_loaded());
  EXPECT_FALSE(engine->last_error().has_value());

  auto cands = engine->QueryCandidates("にほんご", "", kNowBase);
  ASSERT_FALSE(cands.empty());
  EXPECT_EQ(cands.front().surface, "日本語");
  EXPECT_EQ(cands.front().source, azookey::core::CandidateSource::Model);
  EXPECT_NE(cands.front().debug_info.find("zenzai;lp="), std::string::npos);
  EXPECT_NE(cands.front().debug_info.find(";avg="), std::string::npos);
  EXPECT_NE(std::find_if(cands.begin(), cands.end(),
                         [](const auto& candidate) {
                           return candidate.surface == "日本語入力" &&
                                  candidate.source == azookey::core::CandidateSource::Model;
                         }),
            cands.end());
  EXPECT_FALSE(engine->effective_last_error().has_value());

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, LoadedZenzaiRuntimeWithoutMockCandidatesFallsBackOnly) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_no_mock.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_no_mock_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  options.backend = azookey::host::BackendKind::Cpu;
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);

  auto cands = engine->QueryCandidates("にほんご", "", kNowBase);
  ASSERT_FALSE(cands.empty());
  EXPECT_EQ(cands.front().surface, "にほんご");
  EXPECT_NE(cands.front().debug_info.find("zenzai-degraded"), std::string::npos);
  EXPECT_TRUE(std::none_of(cands.begin(), cands.end(), [](const auto& candidate) {
    return candidate.source == azookey::core::CandidateSource::Model;
  }));
  ASSERT_TRUE(engine->effective_last_error().has_value());
  EXPECT_NE(engine->effective_last_error()->find("empty-generation"), std::string::npos);

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, LoadedZenzaiRuntimeDegradesToFallbackAndRecovers) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_degraded.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_degraded_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  EnableMockZenzaiCandidatesForTests(options);
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);
  ASSERT_TRUE(engine->model_loaded());

  auto degraded = engine->QueryCandidates("にほん", "", kNowBase);
  ASSERT_FALSE(degraded.empty());
  EXPECT_NE(degraded.front().debug_info.find("zenzai-degraded"), std::string::npos);
  ASSERT_TRUE(engine->effective_last_error().has_value());
  EXPECT_NE(engine->effective_last_error()->find("empty-generation"), std::string::npos);

  auto recovered = engine->QueryCandidates("にほんご", "", kNowBase);
  ASSERT_FALSE(recovered.empty());
  EXPECT_EQ(recovered.front().surface, "日本語");
  EXPECT_FALSE(engine->effective_last_error().has_value());

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, LiveZenzaiRequestsUseTopOneDecodeLimit) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_live_top_one.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_live_top_one_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  EnableMockZenzaiCandidatesForTests(options);
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);

  auto nbest = engine->QueryCandidates("にほんご", "", kNowBase, nullptr, 10, false);
  ASSERT_GE(nbest.size(), 2u);
  EXPECT_EQ(nbest.front().source, azookey::core::CandidateSource::Model);

  auto live = engine->QueryCandidates("にほんご", "", kNowBase + 1, nullptr, 10, true);
  ASSERT_EQ(live.size(), 1u);
  EXPECT_EQ(live.front().surface, "日本語");
  EXPECT_EQ(live.front().source, azookey::core::CandidateSource::Model);

  auto top_one = engine->QueryCandidates("にほんご", "", kNowBase + 2, nullptr, 1, false);
  ASSERT_EQ(top_one.size(), 1u);
  EXPECT_EQ(top_one.front().surface, "日本語");
  EXPECT_EQ(top_one.front().source, azookey::core::CandidateSource::Model);

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, CanceledZenzaiConvertPreservesDegradedHealth) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_cancel_health.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_cancel_health_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  EnableMockZenzaiCandidatesForTests(options);
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);

  auto degraded = engine->QueryCandidates("にほん", "", kNowBase);
  ASSERT_FALSE(degraded.empty());
  ASSERT_TRUE(engine->effective_last_error().has_value());
  const auto degraded_error = *engine->effective_last_error();
  EXPECT_NE(degraded_error.find("empty-generation"), std::string::npos);

  std::atomic<bool> cancel{false};
  auto canceled = engine->QueryCandidates("きゃんせる", "", kNowBase + 1, &cancel);
  EXPECT_TRUE(cancel.load(std::memory_order_relaxed));
  EXPECT_TRUE(canceled.empty());
  ASSERT_TRUE(engine->effective_last_error().has_value());
  EXPECT_EQ(*engine->effective_last_error(), degraded_error);

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, LoadedZenzaiRuntimeRejectsInvalidUtf8Surface) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_invalid_utf8.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_invalid_utf8_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  EnableMockZenzaiCandidatesForTests(options);
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);
  ASSERT_TRUE(engine->model_loaded());

  auto degraded = engine->QueryCandidates("むこう", "", kNowBase);
  ASSERT_FALSE(degraded.empty());
  EXPECT_EQ(degraded.front().surface, "むこう");
  EXPECT_NE(degraded.front().debug_info.find("zenzai-degraded"), std::string::npos);
  EXPECT_NE(degraded.front().debug_info.find("invalid-utf8-surface"), std::string::npos);
  ASSERT_TRUE(engine->effective_last_error().has_value());
  EXPECT_NE(engine->effective_last_error()->find("invalid-utf8-surface"), std::string::npos);

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, DeadlineBestSoFarTrimsOnlyIncompleteUtf8Suffix) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_deadline_utf8.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_deadline_utf8_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  EnableMockZenzaiCandidatesForTests(options);
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);
  ASSERT_TRUE(engine->model_loaded());

  auto candidates = engine->QueryCandidates("ちゅうだん", "", kNowBase);
  ASSERT_FALSE(candidates.empty());
  EXPECT_EQ(candidates.front().surface, "日本語");
  EXPECT_EQ(candidates.front().source, azookey::core::CandidateSource::Model);
  EXPECT_NE(candidates.front().debug_info.find("utf8-prefix-trimmed"), std::string::npos);
  EXPECT_EQ(candidates.front().debug_info.find("zenzai-degraded"), std::string::npos);
  EXPECT_FALSE(engine->effective_last_error().has_value());

  auto invalid = engine->QueryCandidates("ないぶむこう", "", kNowBase + 1);
  ASSERT_FALSE(invalid.empty());
  EXPECT_EQ(invalid.front().surface, "ないぶむこう");
  EXPECT_NE(invalid.front().debug_info.find("invalid-utf8-surface"), std::string::npos);
  ASSERT_TRUE(engine->effective_last_error().has_value());
  EXPECT_NE(engine->effective_last_error()->find("invalid-utf8-surface"), std::string::npos);

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, ZenzaiCandidateLimitCountsOnlySaneUniqueCandidates) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_sane_unique.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_sane_unique_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  EnableMockZenzaiCandidatesForTests(options);
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);

  auto live = engine->QueryCandidates("せいん", "", kNowBase, nullptr, 1, true);
  ASSERT_EQ(live.size(), 1u);
  EXPECT_EQ(live.front().surface, "正しい");
  EXPECT_EQ(live.front().source, azookey::core::CandidateSource::Model);
  EXPECT_FALSE(engine->effective_last_error().has_value());

  auto nbest = engine->QueryCandidates("じゅうふく", "", kNowBase + 1, nullptr, 2, false);
  ASSERT_EQ(nbest.size(), 2u);
  EXPECT_EQ(nbest.front().surface, "重複");
  EXPECT_EQ(nbest.front().source, azookey::core::CandidateSource::Model);
  EXPECT_NE(std::find_if(nbest.begin(), nbest.end(),
                         [](const auto& candidate) {
                           return candidate.surface == "別候補" &&
                                  candidate.source == azookey::core::CandidateSource::Model;
                         }),
            nbest.end());
  EXPECT_FALSE(engine->effective_last_error().has_value());

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
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

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
  ASSERT_TRUE(result.error.has_value());
  EXPECT_NE(result.error->find("CUDA backend is not linked yet"), std::string::npos);
  EXPECT_EQ(engine->backend(), azookey::host::BackendKind::Cpu);
  EXPECT_EQ(engine->config().model_path, model_path);
  ASSERT_TRUE(engine->config().n_gpu_layers.has_value());
  EXPECT_EQ(engine->config().n_gpu_layers.value(), 35);
  EXPECT_TRUE(engine->model_loaded());
  EXPECT_FALSE(engine->last_error().has_value());

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, LoadModelFailureKeepsPreviouslyLoadedModel) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_reload_failure.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_reload_success_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions good;
  good.path = model_path;
  EnableMockZenzaiCandidatesForTests(good);
  ASSERT_TRUE(engine->LoadModelWithResult(good).ok);
  ASSERT_TRUE(engine->model_loaded());

  azookey::host::ModelLoadOptions bad;
  bad.path = TempPath("azookey_missing_after_success.gguf");
  const auto failed = engine->LoadModelWithResult(bad);
  EXPECT_FALSE(failed.ok);
  EXPECT_TRUE(engine->model_loaded());
  EXPECT_FALSE(engine->last_error().has_value());

  auto cands = engine->QueryCandidates("にほんご", "", kNowBase);
  ASSERT_FALSE(cands.empty());
  EXPECT_EQ(cands.front().surface, "日本語");
  EXPECT_NE(cands.front().debug_info.find("zenzai;lp="), std::string::npos);
  EXPECT_NE(std::find_if(cands.begin(), cands.end(),
                         [](const auto& candidate) {
                           return candidate.surface == "日本語入力" &&
                                  candidate.source == azookey::core::CandidateSource::Model;
                         }),
            cands.end());

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, LoadModelSuccessDoesNotChainWrappers) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_reload_success.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_reload_chain_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  EnableMockZenzaiCandidatesForTests(options);
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);

  auto cands = engine->QueryCandidates("にほん", "", kNowBase);
  ASSERT_FALSE(cands.empty());
  const auto& debug = cands.front().debug_info;
  EXPECT_NE(debug.find("zenzai-degraded"), std::string::npos);
  EXPECT_EQ(CountOccurrences(debug, "zenzai-degraded"), 1u);

  std::remove(model_path.c_str());
  std::remove(lpath);
}

TEST(InferenceEngineTest, UserDictionaryDuplicateKeepsUserSourceOverZenzaiModel) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_user_dict_zenzai_dedup.tsv";
  std::remove(lpath);
  azookey::learning::LearningStore store(lpath);
  auto engine = MakeEngine(store);

  const std::string udict_path =
      (std::filesystem::temp_directory_path() / "azookey_host_engine_user_zenzai.json").string();
  std::remove(udict_path.c_str());
  azookey::learning::UserDictionary dict(udict_path);
  azookey::learning::UserWord w;
  w.word = "日本語";
  w.ruby = "にほんご";
  w.value = -3.0;
  dict.Add(w);
  engine->SetUserDictionary(&dict);

  const std::string model_path = TempPath("azookey_user_dict_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);
  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  EnableMockZenzaiCandidatesForTests(options);
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);

  auto cands = engine->QueryCandidates("にほんご", "", kNowBase);
  const auto user_candidate = std::find_if(cands.begin(), cands.end(), [](const auto& candidate) {
    return candidate.surface == "日本語";
  });
  ASSERT_NE(user_candidate, cands.end());
  EXPECT_EQ(user_candidate->source, azookey::core::CandidateSource::UserDictionary);
  EXPECT_NE(user_candidate->debug_info.find("user-dict"), std::string::npos);
  EXPECT_NE(user_candidate->debug_info.find("dup:zenzai"), std::string::npos);
  EXPECT_NE(std::find_if(cands.begin(), cands.end(),
                         [](const auto& candidate) {
                           return candidate.surface == "日本語入力" &&
                                  candidate.source == azookey::core::CandidateSource::Model;
                         }),
            cands.end());

  std::remove(model_path.c_str());
  std::remove(udict_path.c_str());
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
      options.backend =
          (i % 2 == 0) ? azookey::host::BackendKind::Cpu : azookey::host::BackendKind::Cuda;
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
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

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
  std::thread query_thread(
      [&]() { query_result = engine.QueryCandidates("にほん", "", kNowBase); });

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
