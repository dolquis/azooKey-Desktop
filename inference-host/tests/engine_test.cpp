#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../learning/tests/TestByteCrypto.h"
#include "azookey/core/PlatformPaths.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/SettingsStore.h"
#include "azookey/host/ZenzaiModelConverter.h"
#include "azookey/ipc/Limits.h"
#include "azookey/learning/AtomicFile.h"
#include "azookey/learning/AutoWordStore.h"
#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/TypoCorrectionStore.h"
#include "azookey/learning/UserDictionary.h"

namespace {

using azookey::learning::EncryptedPathFor;

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

class ScopedTempDirectory {
 public:
  ScopedTempDirectory() {
    const auto base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 16; ++attempt) {
      path_ = base / ("azookey_engine_fixture_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "_" + std::to_string(next_id_++));
      if (std::filesystem::create_directory(path_)) return;
    }
    throw std::runtime_error("Could not create a unique engine test directory");
  }

  ~ScopedTempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  ScopedTempDirectory(const ScopedTempDirectory&) = delete;
  ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

  std::string File(const char* name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
  static inline std::atomic<uint64_t> next_id_{0};
};

// Where a LearningStore constructed with `path` saves (the v2 file, .enc).
std::filesystem::path StoredLearningPath(const std::filesystem::path& path) {
  return EncryptedPathFor(azookey::learning::LearningStoreV2PathFor(path));
}

// The constructor still accepts the legacy path, while persistence uses .enc
// (and, for the learning store, the v2 file beside it). Return the removal
// status of the saved file for tests that assert a prior flush.
int RemoveProtectedStoreFile(const std::filesystem::path& path) {
  std::error_code ec;
  bool removed = false;
  for (const auto& base : {path, azookey::learning::LearningStoreV2PathFor(path)}) {
    removed = std::filesystem::remove(EncryptedPathFor(base), ec) || removed;
    std::filesystem::remove(base, ec);
    auto backup = base;
    backup += ".bak";
    std::filesystem::remove(backup, ec);
  }
  return removed ? 0 : -1;
}

bool WaitForFileExists(const std::filesystem::path& target, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::filesystem::exists(target)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return std::filesystem::exists(target);
}
void WriteMinimalGguf(const std::filesystem::path& path, uint32_t version = 3) {
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

class ThrowOnceConverter final : public azookey::core::IConverter {
 public:
  std::vector<azookey::core::Candidate> Convert(const std::string& kana,
                                                const azookey::core::ConversionContext&) override {
    if (!threw_) {
      threw_ = true;
      throw std::runtime_error("first fallback attempt failed");
    }
    return {{kana, kana, 1.0, azookey::core::CandidateSource::Heuristic, "fallback"}};
  }
  std::vector<azookey::core::Candidate> PredictNext(
      const std::string&, const azookey::core::ConversionContext&) override {
    return {};
  }
  std::vector<azookey::core::Candidate> Correct(const std::string&,
                                                const azookey::core::CorrectionHint&,
                                                const azookey::core::ConversionContext&) override {
    return {};
  }
  void Commit(const azookey::core::Candidate&, const azookey::core::ConversionContext&) override {}
  void Learn(const std::string&, const std::string&) override {}

 private:
  bool threw_{false};
};

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

class DeadlineCapturingConverter final : public azookey::core::IConverter {
 public:
  std::vector<azookey::core::Candidate> Convert(
      const std::string& kana, const azookey::core::ConversionContext& context) override {
    saw_deadline_ = context.deadline.has_value();
    if (context.deadline) {
      remaining_budget_ = std::chrono::duration_cast<std::chrono::milliseconds>(
          *context.deadline - std::chrono::steady_clock::now());
    }
    return {azookey::core::Candidate{kana, kana, 1.0, azookey::core::CandidateSource::Model,
                                     "deadline-best-so-far"}};
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

  bool saw_deadline() const { return saw_deadline_; }
  std::chrono::milliseconds remaining_budget() const { return remaining_budget_; }

 private:
  bool saw_deadline_{false};
  std::chrono::milliseconds remaining_budget_{};
};

class ContextCapturingConverter final : public azookey::core::IConverter {
 public:
  std::vector<azookey::core::Candidate> Convert(
      const std::string& kana, const azookey::core::ConversionContext& context) override {
    last_context = context;
    return {{"候補1", kana, 4.0, azookey::core::CandidateSource::Model, "capture-1"},
            {"候補2", kana, 3.0, azookey::core::CandidateSource::Model, "capture-2"},
            {"候補3", kana, 2.0, azookey::core::CandidateSource::Model, "capture-3"}};
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

  azookey::core::ConversionContext last_context;
};

class PredictionConverter final : public azookey::core::IConverter {
 public:
  explicit PredictionConverter(std::vector<azookey::core::Candidate> predictions)
      : predictions_(std::move(predictions)) {}

  std::vector<azookey::core::Candidate> Convert(const std::string&,
                                                const azookey::core::ConversionContext&) override {
    return {};
  }

  std::vector<azookey::core::Candidate> PredictNext(
      const std::string&, const azookey::core::ConversionContext&) override {
    return predictions_;
  }

  std::vector<azookey::core::Candidate> Correct(const std::string&,
                                                const azookey::core::CorrectionHint&,
                                                const azookey::core::ConversionContext&) override {
    return {};
  }

  void Commit(const azookey::core::Candidate&, const azookey::core::ConversionContext&) override {}
  void Learn(const std::string&, const std::string&) override {}

 private:
  std::vector<azookey::core::Candidate> predictions_;
};

class ThrowingLearningStore final : public azookey::learning::LearningStore {
 public:
  ThrowingLearningStore()
      : azookey::learning::LearningStore(TempPath("azookey_host_engine_reranker_throw.tsv"),
                                         &azookey::learning::test::Crypto()) {}

  double Score(const std::string&, const std::string&, uint64_t) const override {
    throw std::runtime_error("candidate text private-score-failure");
  }
};

class CancelOnScoreLearningStore final : public azookey::learning::LearningStore {
 public:
  explicit CancelOnScoreLearningStore(std::atomic<bool>& cancel)
      : azookey::learning::LearningStore(TempPath("azookey_host_rerank_cancel.tsv"),
                                         &azookey::learning::test::Crypto()),
        cancel_(cancel) {}

  double Score(const std::string&, const std::string&, uint64_t) const override {
    cancel_.store(true, std::memory_order_relaxed);
    return 0.0;
  }

 private:
  std::atomic<bool>& cancel_;
};
}  // namespace

TEST(InferenceEngineTest, LocalAiCleanupRealModelSmoke) {
#ifndef AZOOKEY_AI_TEST_MODEL
  GTEST_SKIP() << "Requires llama.cpp and an explicit AZOOKEY_ZENZAI_TEST_MODEL";
#else
  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), nullptr,
                                        {});
  azookey::host::ModelLoadOptions model;
  model.path = AZOOKEY_AI_TEST_MODEL;
  ASSERT_TRUE(engine.LoadModel(model));
  azookey::host::AiTransformRequest request;
  request.task = azookey::host::AiTask::Cleanup;
  request.ai_allowed = true;
  request.text = "きょうはいいてんきです";
  request.raw_romaji = "kyouhaiitenkidesu";
  azookey::host::AiBackendOptions options;
  options.backend = "local-zenzai";
  const auto result = azookey::host::AiBackend().Transform(
      request, options, nullptr, [&](const auto& input, const auto* cancel, auto deadline) {
        return engine.TransformLocal(input, cancel, deadline);
      });
  ASSERT_TRUE(result.ok) << static_cast<int>(result.error_class);
  EXPECT_FALSE(result.result.empty());
  EXPECT_EQ(result.result.find("。"), std::string::npos);
  RecordProperty("cleanup_result", result.result);
#endif
}

TEST(InferenceEngineTest, QueryWithLearningBoost) {
  const char* path = "azookey_host_engine_learning.tsv";
  RemoveProtectedStoreFile(path);
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());

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
  RemoveProtectedStoreFile(path);
}

TEST(InferenceEngineTest, QueryAppliesConfiguredCandidateAndUnicodeContextLimits) {
  auto converter = std::make_unique<ContextCapturingConverter>();
  auto* capture = converter.get();
  azookey::host::EngineConfig config;
  config.max_candidates = 2;
  config.max_context_length = 3;
  azookey::host::InferenceEngine engine(std::move(converter), nullptr, config);

  const auto candidates = engine.QueryCandidates("かな", "prefix日本語", kNowBase);

  EXPECT_EQ(capture->last_context.preceding_text, "日本語");
  EXPECT_EQ(capture->last_context.max_candidates, 2u);
  ASSERT_EQ(candidates.size(), 2u);
  EXPECT_EQ(candidates[0].surface, "候補1");
  EXPECT_EQ(candidates[1].surface, "候補2");
}

TEST(InferenceEngineTest, RequestCandidateLimitCannotExceedConfiguredLimit) {
  auto converter = std::make_unique<ContextCapturingConverter>();
  auto* capture = converter.get();
  azookey::host::EngineConfig config;
  config.max_candidates = 2;
  azookey::host::InferenceEngine engine(std::move(converter), nullptr, config);

  const auto candidates = engine.QueryCandidates("かな", "", kNowBase, nullptr, 10);

  EXPECT_EQ(capture->last_context.max_candidates, 2u);
  EXPECT_EQ(candidates.size(), 2u);
}

TEST(InferenceEngineTest, ConfiguredCandidateLimitCanExceedLegacyTipLimit) {
  auto converter = std::make_unique<ContextCapturingConverter>();
  auto* capture = converter.get();
  azookey::host::EngineConfig config;
  config.max_candidates = 12;
  azookey::host::InferenceEngine engine(std::move(converter), nullptr, config);

  (void)engine.QueryCandidates("かな", "", kNowBase, nullptr, 20);

  EXPECT_EQ(capture->last_context.max_candidates, 12u);
}

TEST(InferenceEngineTest, CommitObservationDebouncesUntilCountThreshold) {
  const std::string path = TempPath("azookey_host_engine_learning_debounce_count.tsv");
  RemoveProtectedStoreFile(path);
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 3;
  cfg.learning_flush_interval_sec = 1000;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  engine->CommitObservation("reading", "surface", kNowBase + 1);
  ASSERT_TRUE(std::filesystem::exists(StoredLearningPath(path)));
  ASSERT_EQ(RemoveProtectedStoreFile(path), 0);
  engine->CommitObservation("reading", "surface", kNowBase + 2);
  engine->CommitObservation("reading", "surface", kNowBase + 3);
  EXPECT_FALSE(std::filesystem::exists(StoredLearningPath(path)));
  EXPECT_TRUE(store.dirty());

  engine->CommitObservation("reading", "surface", kNowBase + 4);
  EXPECT_TRUE(std::filesystem::exists(StoredLearningPath(path)));
  EXPECT_FALSE(store.dirty());

  engine.reset();
  RemoveProtectedStoreFile(path);
}

// DEV-554: a CommitObservation resent after a pipe drop carries the same
// observation_id, so the Host must apply it exactly once.
TEST(InferenceEngineTest, CommitObservationIgnoresRepeatedObservationId) {
  const std::string path = TempPath("azookey_host_engine_observation_id_dedupe.tsv");
  RemoveProtectedStoreFile(path);
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 100;
  cfg.learning_flush_interval_sec = 1000;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  EXPECT_TRUE(engine->CommitObservation("にほん", "二本", kNowBase + 1, "tip-a:1"));
  const double after_first = store.Score("にほん", "二本", kNowBase + 1);

  EXPECT_FALSE(engine->CommitObservation("にほん", "二本", kNowBase + 1, "tip-a:1"));
  EXPECT_DOUBLE_EQ(store.Score("にほん", "二本", kNowBase + 1), after_first);

  // A different id is a different commit and must still be counted.
  EXPECT_TRUE(engine->CommitObservation("にほん", "二本", kNowBase + 1, "tip-a:2"));
  EXPECT_GT(store.Score("にほん", "二本", kNowBase + 1), after_first);

  engine.reset();
  RemoveProtectedStoreFile(path);
}

// A TIP that predates DEV-554 sends no observation_id. Those observations carry
// no dedupe information, so every one of them must still be applied.
TEST(InferenceEngineTest, CommitObservationWithoutObservationIdIsNeverDeduped) {
  const std::string path = TempPath("azookey_host_engine_observation_id_absent.tsv");
  RemoveProtectedStoreFile(path);
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 100;
  cfg.learning_flush_interval_sec = 1000;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  EXPECT_TRUE(engine->CommitObservation("にほん", "二本", kNowBase + 1, ""));
  const double after_first = store.Score("にほん", "二本", kNowBase + 1);
  EXPECT_TRUE(engine->CommitObservation("にほん", "二本", kNowBase + 1, ""));
  EXPECT_GT(store.Score("にほん", "二本", kNowBase + 1), after_first);

  engine.reset();
  RemoveProtectedStoreFile(path);
}

// The dedupe ring is bounded, so an id evicted by newer traffic is applied
// again. The bound is what keeps a long-lived Host from growing without limit.
TEST(InferenceEngineTest, CommitObservationDedupeRingEvictsOldestIds) {
  const std::string path = TempPath("azookey_host_engine_observation_id_evict.tsv");
  RemoveProtectedStoreFile(path);
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 100000;
  cfg.learning_flush_interval_sec = 100000;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  ASSERT_TRUE(engine->CommitObservation("にほん", "二本", kNowBase + 1, "tip-a:1"));
  // The ring covers every connected TIP's resend backlog at once; that many
  // distinct ids push "tip-a:1" out of it.
  const size_t ring_depth = static_cast<size_t>(azookey::ipc::kMaxPipeInstances) *
                            azookey::ipc::kMaxQueuedCommitObservations;
  for (size_t i = 0; i < ring_depth; ++i) {
    ASSERT_TRUE(engine->CommitObservation("かな", "仮名", kNowBase + 1,
                                          "tip-a:evict-" + std::to_string(i)));
  }
  EXPECT_TRUE(engine->CommitObservation("にほん", "二本", kNowBase + 1, "tip-a:1"));

  engine.reset();
  RemoveProtectedStoreFile(path);
}

// DEV-554: one engine serves every connection, so a second TIP replaying its own
// backlog after a Host restart must not evict the first TIP's applied ids before
// that TIP gets to resend them.
TEST(InferenceEngineTest, CommitObservationDedupeSurvivesAnotherTipsFullBacklog) {
  const std::string path = TempPath("azookey_host_engine_observation_id_multi_tip.tsv");
  RemoveProtectedStoreFile(path);
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 100000;
  cfg.learning_flush_interval_sec = 100000;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  ASSERT_TRUE(engine->CommitObservation("にほん", "二本", kNowBase + 1, "tip-a:1"));

  // Every other TIP the transport admits replays a full backlog.
  for (uint32_t tip = 1; tip < azookey::ipc::kMaxPipeInstances; ++tip) {
    for (size_t i = 0; i < azookey::ipc::kMaxQueuedCommitObservations; ++i) {
      ASSERT_TRUE(engine->CommitObservation(
          "かな", "仮名", kNowBase + 1, "tip-" + std::to_string(tip) + ":" + std::to_string(i)));
    }
  }

  // tip-a:1 is still covered, so its own resend is discarded as a duplicate.
  EXPECT_FALSE(engine->CommitObservation("にほん", "二本", kNowBase + 1, "tip-a:1"));

  engine.reset();
  RemoveProtectedStoreFile(path);
}

TEST(InferenceEngineTest, CommitObservationFlushesAfterInterval) {
  const std::string path = TempPath("azookey_host_engine_learning_debounce_interval.tsv");
  RemoveProtectedStoreFile(path);
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 100;
  cfg.learning_flush_interval_sec = 5;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  engine->CommitObservation("initial", "saved", kNowBase + 1);
  ASSERT_TRUE(std::filesystem::exists(StoredLearningPath(path)));
  ASSERT_EQ(RemoveProtectedStoreFile(path), 0);
  engine->CommitObservation("reading", "surface", kNowBase + 10);
  engine->CommitObservation("reading", "surface", kNowBase + 14);
  EXPECT_FALSE(std::filesystem::exists(StoredLearningPath(path)));

  engine->CommitObservation("reading", "surface", kNowBase + 15);
  EXPECT_TRUE(std::filesystem::exists(StoredLearningPath(path)));
  EXPECT_FALSE(store.dirty());

  engine.reset();
  RemoveProtectedStoreFile(path);
}

TEST(InferenceEngineTest, CommitObservationFlushesAfterIntervalWithoutAnotherObservation) {
  const std::string path = TempPath("azookey_host_engine_learning_idle_interval.tsv");
  RemoveProtectedStoreFile(path);
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 100;
  cfg.learning_flush_interval_sec = 1;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  engine->CommitObservation("initial", "saved", kNowBase + 1);
  ASSERT_TRUE(std::filesystem::exists(StoredLearningPath(path)));
  ASSERT_EQ(RemoveProtectedStoreFile(path), 0);
  engine->CommitObservation("reading", "surface", kNowBase + 10);
  EXPECT_FALSE(std::filesystem::exists(StoredLearningPath(path)));
  EXPECT_TRUE(WaitForFileExists(StoredLearningPath(path), std::chrono::milliseconds(2500)));

  // The flush renames a complete temp file into place, so a failed Load here
  // is an open that lost to another handle on the fresh file (the rename
  // itself, or a scanner on CI runners). Retry the open instead of reading once.
  azookey::learning::LearningStore loaded(path, &azookey::learning::test::Crypto());
  const auto load_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
  bool load_ok = loaded.Load();
  while (!load_ok && std::chrono::steady_clock::now() < load_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    load_ok = loaded.Load();
  }
  ASSERT_TRUE(load_ok);
  EXPECT_GT(loaded.Score("reading", "surface", kNowBase + 10), 0.0);

  engine.reset();
  RemoveProtectedStoreFile(path);
}

TEST(InferenceEngineTest, FlushLearningStorePersistsPendingObservation) {
  const std::string path = TempPath("azookey_host_engine_learning_flush.tsv");
  RemoveProtectedStoreFile(path);
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 100;
  cfg.learning_flush_interval_sec = 1000;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  engine->CommitObservation("reading", "surface", kNowBase + 1);
  ASSERT_TRUE(std::filesystem::exists(StoredLearningPath(path)));
  ASSERT_EQ(RemoveProtectedStoreFile(path), 0);
  engine->CommitObservation("pending", "observation", kNowBase + 2);
  EXPECT_FALSE(std::filesystem::exists(StoredLearningPath(path)));
  EXPECT_TRUE(engine->FlushLearningStore());
  EXPECT_TRUE(std::filesystem::exists(StoredLearningPath(path)));
  EXPECT_FALSE(store.dirty());

  engine.reset();
  RemoveProtectedStoreFile(path);
}

TEST(InferenceEngineTest, PrunesLearningStoreOnlyAtFlushBoundary) {
  const std::string path = TempPath("azookey_host_engine_learning_prune_on_flush.tsv");
  RemoveProtectedStoreFile(path);
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 100;
  cfg.learning_flush_interval_sec = 1000;
  cfg.learning_max_records = 1;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  engine->CommitObservation("a", "first", kNowBase + 1);
  ASSERT_TRUE(std::filesystem::exists(StoredLearningPath(path)));
  ASSERT_EQ(RemoveProtectedStoreFile(path), 0);
  engine->CommitObservation("b", "second", kNowBase + 2);
  engine->CommitObservation("b", "second", kNowBase + 3);
  EXPECT_EQ(store.size(), 2u);
  EXPECT_FALSE(std::filesystem::exists(StoredLearningPath(path)));

  EXPECT_TRUE(engine->FlushLearningStore());
  EXPECT_EQ(store.size(), 1u);
  EXPECT_EQ(store.Score("a", "first", kNowBase + 3), 0.0);
  EXPECT_GT(store.Score("b", "second", kNowBase + 3), 0.0);
  EXPECT_TRUE(std::filesystem::exists(StoredLearningPath(path)));

  engine.reset();
  RemoveProtectedStoreFile(path);
}

TEST(InferenceEngineTest, SaveFailureKeepsDirtyStateAndCanRetry) {
  const auto root =
      std::filesystem::temp_directory_path() / "azookey_host_engine_learning_save_failure";
  const auto blocked_path = root / "learning-as-directory.tsv";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(azookey::learning::LearningStoreV2PathFor(blocked_path));
  azookey::learning::LearningStore store(blocked_path.string(), &azookey::learning::test::Crypto());

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
  EXPECT_TRUE(std::filesystem::exists(StoredLearningPath(blocked_path)));

  engine.reset();
  std::filesystem::remove_all(root);
}

#ifdef _WIN32
TEST(InferenceEngineTest, ShutdownFlushRecoversAfterTemporaryReplaceBlocker) {
  ScopedTempDirectory directory;
  const auto path = directory.File("learning.tsv");
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());
  azookey::host::EngineConfig cfg;
  cfg.learning_flush_every_n = 100;
  cfg.learning_flush_interval_sec = 1000;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);
  engine->CommitObservation("first", "saved", kNowBase + 1);
  engine->CommitObservation("pending", "observation", kNowBase + 2);
  ASSERT_TRUE(store.dirty());
  const auto encrypted = StoredLearningPath(path);
  HANDLE reader = CreateFileW(encrypted.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(reader, INVALID_HANDLE_VALUE);
  auto shutdown =
      std::async(std::launch::async, [engine = std::move(engine)]() mutable { engine.reset(); });
  // The old destructor exhausts all three one-shot saves in about 100 ms.
  // Hold the sharing conflict beyond that window, then let shutdown complete.
  EXPECT_EQ(shutdown.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
  CloseHandle(reader);
  shutdown.get();
  EXPECT_FALSE(store.dirty());
  azookey::learning::LearningStore loaded(path, &azookey::learning::test::Crypto());
  ASSERT_TRUE(loaded.Load());
  EXPECT_GT(loaded.Score("pending", "observation", kNowBase + 2), 0.0);
}

TEST(InferenceEngineTest, ObservationFlushDoesNotWaitOutReplaceBlocker) {
  ScopedTempDirectory directory;
  const auto path = directory.File("learning.tsv");
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());
  azookey::host::EngineConfig cfg;
  cfg.learning_flush_every_n = 1;
  cfg.learning_flush_interval_sec = 1000;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);
  engine->CommitObservation("first", "saved", kNowBase + 1);
  ASSERT_FALSE(store.dirty());
  const auto encrypted = StoredLearningPath(path);
  HANDLE reader = CreateFileW(encrypted.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(reader, INVALID_HANDLE_VALUE);
  testing::internal::CaptureStderr();
  const auto start = std::chrono::steady_clock::now();
  engine->CommitObservation("pending", "observation", kNowBase + 2);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  (void)testing::internal::GetCapturedStderr();
  // The flush runs under the lock queries wait on, so it must not spend the
  // 500 ms retry budget that shutdown and explicit flushes use.
  EXPECT_LT(elapsed, std::chrono::milliseconds(400));
  EXPECT_TRUE(store.dirty());
  CloseHandle(reader);
  EXPECT_TRUE(engine->FlushLearningStore());
  EXPECT_FALSE(store.dirty());
}
#endif

TEST(InferenceEngineTest, BurstStartFlushPersistsFirstObservationWithoutExplicitFlush) {
  const std::string path = TempPath("azookey_host_engine_learning_burst_start.tsv");
  RemoveProtectedStoreFile(path);
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 100;
  cfg.learning_flush_interval_sec = 1000;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  engine->CommitObservation("reading", "surface", kNowBase + 1);
  EXPECT_TRUE(std::filesystem::exists(StoredLearningPath(path)));
  EXPECT_FALSE(store.dirty());

  azookey::learning::LearningStore loaded(path, &azookey::learning::test::Crypto());
  ASSERT_TRUE(loaded.Load());
  EXPECT_GT(loaded.Score("reading", "surface", kNowBase + 1), 0.0);

  engine.reset();
  RemoveProtectedStoreFile(path);
}

TEST(InferenceEngineTest, BurstStartFlushIsRateLimitedWithinInterval) {
  const std::string path = TempPath("azookey_host_engine_learning_burst_rate_limit.tsv");
  RemoveProtectedStoreFile(path);
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.learning_alpha = 0.8;
  cfg.learning_flush_every_n = 100;
  cfg.learning_flush_interval_sec = 1000;
  cfg.learning_min_weight = 0.0;
  auto engine = MakeEngine(store, cfg);

  engine->CommitObservation("first", "saved", kNowBase + 1);
  ASSERT_TRUE(std::filesystem::exists(StoredLearningPath(path)));
  ASSERT_EQ(RemoveProtectedStoreFile(path), 0);

  engine->CommitObservation("second", "pending", kNowBase + 2);
  EXPECT_FALSE(std::filesystem::exists(StoredLearningPath(path)));
  EXPECT_TRUE(store.dirty());

  engine.reset();
  RemoveProtectedStoreFile(path);
}

TEST(InferenceEngineTest, UserDictionaryInjection) {
  const char* lpath = "azookey_host_engine_user_dict_learn.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);

  const std::string udict_path =
      (std::filesystem::temp_directory_path() / "azookey_host_engine_user.json").string();
  azookey::learning::UserDictionary dict(udict_path, &azookey::learning::test::Crypto());
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

  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, UserDictionarySwitchControlsCandidatesPredictionsAndReverseConversion) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::UserDictionary dictionary(temp.File("user.json"),
                                               &azookey::learning::test::Crypto());
  azookey::learning::UserWord word;
  word.word = "試験専用語";
  word.ruby = "しけんせんようご";
  ASSERT_TRUE(dictionary.Add(word));
  azookey::host::EngineConfig config;
  config.dictionary.user_dictionary_enabled = false;
  auto engine = MakeEngine(store, config);
  engine->SetUserDictionary(&dictionary);
  const auto contains_word = [&](const auto& candidates) {
    return std::any_of(candidates.begin(), candidates.end(),
                       [&](const auto& candidate) { return candidate.surface == word.word; });
  };
  EXPECT_FALSE(contains_word(engine->QueryCandidates(word.ruby, "", kNowBase)));
  EXPECT_FALSE(contains_word(engine->QueryPredictions("しけん", "", kNowBase)));
  EXPECT_TRUE(engine->ReverseConvert(word.word, kNowBase).empty());
  EXPECT_EQ(dictionary.Size(), 1u);

  config.dictionary.user_dictionary_enabled = true;
  engine->ApplyConfig(config);
  EXPECT_TRUE(contains_word(engine->QueryCandidates(word.ruby, "", kNowBase)));
  EXPECT_TRUE(contains_word(engine->QueryPredictions("しけん", "", kNowBase)));
  EXPECT_EQ(engine->ReverseConvert(word.word, kNowBase), word.ruby);

  config.dictionary.user_dictionary_enabled = false;
  engine->ApplyConfig(config);
  EXPECT_FALSE(contains_word(engine->QueryCandidates(word.ruby, "", kNowBase)));
  EXPECT_FALSE(contains_word(engine->QueryPredictions("しけん", "", kNowBase)));
  EXPECT_TRUE(engine->ReverseConvert(word.word, kNowBase).empty());
  EXPECT_EQ(dictionary.Size(), 1u);
}

TEST(InferenceEngineTest, AddUserWordReloadsDiskBeforeSaving) {
  const std::string lpath = TempPath("azookey_host_engine_user_dict_reload_add.tsv");
  const std::string udict_path = TempPath("azookey_host_engine_user_dict_reload_add.json");
  RemoveProtectedStoreFile(lpath);
  RemoveProtectedStoreFile(udict_path);

  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);
  azookey::learning::UserDictionary dict(udict_path, &azookey::learning::test::Crypto());
  ASSERT_TRUE(dict.Load());
  engine->SetUserDictionary(&dict);

  azookey::learning::UserWord external;
  external.word = "External";
  external.ruby = "external";
  {
    azookey::learning::UserDictionary writer(udict_path, &azookey::learning::test::Crypto());
    ASSERT_TRUE(writer.Load());
    writer.Add(external);
    ASSERT_TRUE(writer.Save());
  }

  azookey::learning::UserWord added;
  added.word = "Added";
  added.ruby = "added";
  ASSERT_TRUE(engine->AddUserWord(added));

  azookey::learning::UserDictionary loaded(udict_path, &azookey::learning::test::Crypto());
  ASSERT_TRUE(loaded.Load());
  EXPECT_EQ(loaded.Lookup("external").size(), 1u);
  EXPECT_EQ(loaded.Lookup("added").size(), 1u);

  RemoveProtectedStoreFile(udict_path);
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, RemoveUserWordReloadsDiskBeforeSaving) {
  const std::string lpath = TempPath("azookey_host_engine_user_dict_reload_remove.tsv");
  const std::string udict_path = TempPath("azookey_host_engine_user_dict_reload_remove.json");
  RemoveProtectedStoreFile(lpath);
  RemoveProtectedStoreFile(udict_path);

  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);
  azookey::learning::UserDictionary dict(udict_path, &azookey::learning::test::Crypto());
  ASSERT_TRUE(dict.Load());
  engine->SetUserDictionary(&dict);

  azookey::learning::UserWord external;
  external.word = "External";
  external.ruby = "external";
  {
    azookey::learning::UserDictionary writer(udict_path, &azookey::learning::test::Crypto());
    ASSERT_TRUE(writer.Load());
    writer.Add(external);
    ASSERT_TRUE(writer.Save());
  }

  ASSERT_TRUE(engine->RemoveUserWord("External", "external"));

  azookey::learning::UserDictionary loaded(udict_path, &azookey::learning::test::Crypto());
  ASSERT_TRUE(loaded.Load());
  EXPECT_TRUE(loaded.Lookup("external").empty());

  RemoveProtectedStoreFile(udict_path);
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, CancelEarlyReturn) {
  const char* lpath = "azookey_host_engine_cancel.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);

  std::atomic<bool> cancel{true};
  auto cands = engine->QueryCandidates("にほん", "", kNowBase, &cancel);
  EXPECT_TRUE(cands.empty());

  cancel.store(false);
  auto cands2 = engine->QueryCandidates("にほん", "", kNowBase, &cancel);
  EXPECT_FALSE(cands2.empty());

  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, LegacyOverloadStillWorks) {
  const char* lpath = "azookey_host_engine_legacy.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);

  // Three-argument overload exists for backwards compatibility with main.cpp
  // and the existing bench harness.
  auto cands = engine->QueryCandidates("わたし", "", kNowBase);
  EXPECT_FALSE(cands.empty());
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, RerankerFailureFallsBackToRawCandidates) {
  ThrowingLearningStore store;
  auto engine = MakeEngine(store);

  auto candidates = engine->QueryCandidates("にほん", "", kNowBase);
  ASSERT_FALSE(candidates.empty());
  EXPECT_EQ(candidates.front().surface, "日本");
  ASSERT_TRUE(engine->last_error().has_value());
  ASSERT_TRUE(engine->effective_last_error().has_value());
  EXPECT_EQ(*engine->last_error(), "reranker failed");
  EXPECT_EQ(engine->last_error()->find("private-score-failure"), std::string::npos);

  auto predictions = engine->QueryPredictions("にほん", "", kNowBase);
  ASSERT_FALSE(predictions.empty());
  EXPECT_NE(predictions.front().debug_info.find("predict"), std::string::npos);

  auto corrections = engine->QueryCorrections("にほん", "", "日本", kNowBase);
  ASSERT_FALSE(corrections.empty());
  EXPECT_NE(corrections.front().surface, "日本");
  ASSERT_TRUE(engine->last_error().has_value());
  EXPECT_NE(engine->last_error()->find("reranker failed"), std::string::npos);
}

TEST(InferenceEngineTest, PredictionsPrependLearningDeduplicateAndCapDisplayCount) {
  const auto path = TempPath("azookey_host_prediction_learning.tsv");
  RemoveProtectedStoreFile(path);
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());
  store.Observe("にほんご", "日本語", 5.0, kNowBase);
  store.Observe("にほんじん", "日本人", 4.0, kNowBase);
  store.Observe("にほんしゅ", "日本酒", 3.0, kNowBase);
  store.Observe("にほんしょ", "日本書", 2.0, kNowBase);

  std::vector<azookey::core::Candidate> converter_predictions = {
      {"日本語", "にほんご", 100.0, azookey::core::CandidateSource::Model, "duplicate"},
      {"モデル1", "もでる1", 4.0, azookey::core::CandidateSource::Model, "model-1"},
      {"モデル2", "もでる2", 3.0, azookey::core::CandidateSource::Model, "model-2"},
      {"モデル3", "もでる3", 2.0, azookey::core::CandidateSource::Model, "model-3"},
  };
  azookey::host::EngineConfig config;
  config.prediction_learning_max_entries = 3;
  auto engine = std::make_unique<azookey::host::InferenceEngine>(
      std::make_unique<PredictionConverter>(std::move(converter_predictions)), &store, config);

  const auto predictions = engine->QueryPredictions("にほん", "", kNowBase);
  ASSERT_EQ(predictions.size(), 5u);
  EXPECT_EQ(predictions[0].surface, "日本語");
  EXPECT_EQ(predictions[1].surface, "日本人");
  EXPECT_EQ(predictions[2].surface, "日本酒");
  EXPECT_EQ(predictions[3].surface, "モデル1");
  EXPECT_EQ(predictions[4].surface, "モデル2");
  EXPECT_EQ(predictions[0].reading, "にほんご");
  EXPECT_EQ(predictions[0].source, azookey::core::CandidateSource::Learning);
  EXPECT_EQ(std::count_if(predictions.begin(), predictions.end(),
                          [](const auto& candidate) { return candidate.surface == "日本語"; }),
            1);

  engine.reset();
  RemoveProtectedStoreFile(path);
}

TEST(InferenceEngineTest, PredictionsExcludeExactReadingAndApplyUpdatedLearningConfig) {
  const auto path = TempPath("azookey_host_prediction_exact.tsv");
  RemoveProtectedStoreFile(path);
  azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());
  store.Observe("にほん", "完全一致", 10.0, kNowBase);
  store.Observe("にほんご", "日本語", 1.0, kNowBase);
  store.Observe("にほんじん", "日本人", 0.5, kNowBase);

  azookey::host::EngineConfig config;
  config.prediction_learning_max_entries = 3;
  auto engine = std::make_unique<azookey::host::InferenceEngine>(
      std::make_unique<PredictionConverter>(std::vector<azookey::core::Candidate>{}), &store,
      config);

  azookey::host::EngineConfig updated = config;
  updated.prediction_learning_max_entries = 2;
  updated.prediction_learning_min_score = 0.75;
  engine->ApplyConfig(updated);
  const auto predictions = engine->QueryPredictions("にほん", "", kNowBase);
  ASSERT_EQ(predictions.size(), 1u);
  EXPECT_EQ(predictions.front().surface, "日本語");
  EXPECT_EQ(predictions.front().reading, "にほんご");

  engine.reset();
  RemoveProtectedStoreFile(path);
}

TEST(InferenceEngineTest, LoadModelFallbackWithoutPath) {
  const char* lpath = "azookey_host_engine_load_empty.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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

  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, LoadModelRecordsOptionsAndMissingPath) {
  const char* lpath = "azookey_host_engine_load_missing.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
  azookey::host::EngineConfig config;
  config.inference_threads = 6;
  auto engine = MakeEngine(store, config);

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
  EXPECT_EQ(engine->config().inference_threads, 6);
  EXPECT_FALSE(engine->model_loaded());

  auto cands = engine->QueryCandidates("にほん", "", kNowBase);
  ASSERT_FALSE(cands.empty());
  EXPECT_EQ(cands.front().surface, "日本");

  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, ResolvedPowerProfileThreadsSurviveRuntimeAndModelReloads) {
  using namespace azookey::host;
  azookey::learning::LearningStore store(TempPath("azookey_power_profile_reload.tsv"),
                                         &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);
  RuntimeSettings settings;
  int queries = 0;
  PowerSource source = PowerSource::Ac;
  const auto provider = [&] {
    ++queries;
    return InferenceThreadEnvironment{source, 6};
  };
  for (const auto next_source : {PowerSource::Ac, PowerSource::Battery, PowerSource::Unknown}) {
    source = next_source;
    const int32_t expected = source == PowerSource::Ac ? 6 : source == PowerSource::Battery ? 2 : 4;
    const auto next =
        ApplyRuntimeSettingsToEngineConfig(engine->config(), settings, BackendKind::Cpu, provider);
    engine->ApplyConfig(next);
    EXPECT_EQ(engine->config().inference_threads, expected);
    const int queries_before_reload = queries;
    source = PowerSource::Battery;
    ASSERT_TRUE(engine->LoadModelWithResult().ok);
    EXPECT_EQ(engine->config().inference_threads, expected);
    ModelLoadOptions options;
    ASSERT_TRUE(engine->LoadModelWithResult(options).ok);
    EXPECT_EQ(engine->config().inference_threads, expected);
    EXPECT_EQ(queries, queries_before_reload);
  }
  EXPECT_EQ(queries, 3);
}

TEST(InferenceEngineTest, StartModelPreloadKeepsFallbackResponsiveWhileLoadIsPending) {
  using namespace std::chrono_literals;

  const char* lpath = "azookey_host_engine_preload_pending.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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

  RemoveProtectedStoreFile(lpath);
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

TEST(InferenceEngineTest, ProbeZenzaiGgufModelKeepsNonAsciiPathAsUtf8) {
  // UTF-8 bytes for 日本語モデル.gguf, written as escapes so the fixture pins the
  // bytes that --model and model.selectedPath deliver.
  constexpr const char* kModelName =
      "azookey_probe_\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x83\xa2\xe3\x83\x87\xe3\x83\xab.gguf";
  const auto model_file =
      std::filesystem::temp_directory_path() / azookey::core::Utf8Path(kModelName);
  std::filesystem::remove(model_file);
  WriteMinimalGguf(model_file);
  // path::string() would encode with the active code page on Windows.
  const auto model_path = azookey::core::PathToUtf8(model_file);

  const auto result = azookey::host::ProbeZenzaiGgufModel(model_path);
  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.info.path, model_path);
  EXPECT_EQ(result.info.file_size_bytes, 8u);
  EXPECT_EQ(result.info.gguf_version, 3u);

  std::filesystem::remove(model_file);
}

TEST(InferenceEngineTest, LoadModelAcceptsNonAsciiUtf8Path) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_load_non_ascii.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);

  // UTF-8 bytes for 日本語モデル.gguf.
  constexpr const char* kModelName =
      "azookey_load_\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x83\xa2\xe3\x83\x87\xe3\x83\xab.gguf";
  const auto model_file =
      std::filesystem::temp_directory_path() / azookey::core::Utf8Path(kModelName);
  std::filesystem::remove(model_file);
  WriteMinimalGguf(model_file);

  azookey::host::ModelLoadOptions options;
  options.path = azookey::core::PathToUtf8(model_file);
  options.backend = azookey::host::BackendKind::Cpu;
  EnableMockZenzaiCandidatesForTests(options);

  const auto result = engine->LoadModelWithResult(options);
  EXPECT_TRUE(result.ok) << result.error.value_or("");
  EXPECT_TRUE(engine->model_loaded());
  EXPECT_EQ(engine->config().model_path, options.path);

  std::filesystem::remove(model_file);
  RemoveProtectedStoreFile(lpath);
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
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_invalid_llama_model.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  options.backend = azookey::host::BackendKind::Cpu;
  const auto result = engine->LoadModelWithResult(options);

  std::remove(model_path.c_str());
  RemoveProtectedStoreFile(lpath);

  EXPECT_FALSE(result.ok);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(*result.error, "model load failed");
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

TEST(InferenceEngineTest, ResolvesOnlyMisdeclaredZenzaiEosFromVocabulary) {
  const std::vector<std::string> shifted_vocabulary{"[PAD]", "unused", "<s>", "unused-2", "</s>"};
  const auto resolved = azookey::host::ResolveZenzaiEosTokenOverride(2, shifted_vocabulary);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(*resolved, 4u);

  EXPECT_FALSE(azookey::host::ResolveZenzaiEosTokenOverride(4, shifted_vocabulary).has_value());
  EXPECT_FALSE(azookey::host::ResolveZenzaiEosTokenOverride(1, shifted_vocabulary).has_value());
  EXPECT_FALSE(azookey::host::ResolveZenzaiEosTokenOverride(99, shifted_vocabulary).has_value());
  EXPECT_FALSE(azookey::host::ResolveZenzaiEosTokenOverride(
                   2, std::vector<std::string>{"[PAD]", "unused", "<s>"})
                   .has_value());
}

TEST(InferenceEngineTest, BuildsPreTokenizerKvOverrideForZenzaiGguf) {
  azookey::host::ZenzaiTokenizerMetadata metadata;
  metadata.pre_tokenizer = "gpt2-small-japanese-char";

  const auto overrides = azookey::host::BuildZenzaiKvOverrides(metadata);

  ASSERT_EQ(overrides.size(), 1u);
  EXPECT_EQ(overrides[0].key, "tokenizer.ggml.pre");
  EXPECT_EQ(overrides[0].type, azookey::host::ZenzaiKvOverride::Type::String);
  EXPECT_EQ(overrides[0].string_value, "gpt-2");
}

TEST(InferenceEngineTest, BuildsNoPreTokenizerKvOverrideForOtherGguf) {
  azookey::host::ZenzaiTokenizerMetadata known_pre_tokenizer;
  known_pre_tokenizer.pre_tokenizer = "gpt-2";
  EXPECT_TRUE(azookey::host::BuildZenzaiKvOverrides(known_pre_tokenizer).empty());

  azookey::host::ZenzaiTokenizerMetadata unrelated_pre_tokenizer;
  unrelated_pre_tokenizer.pre_tokenizer = "llama-bpe";
  EXPECT_TRUE(azookey::host::BuildZenzaiKvOverrides(unrelated_pre_tokenizer).empty());

  // A GGUF that declares no pre-tokenizer at all is loaded as-is.
  EXPECT_TRUE(
      azookey::host::BuildZenzaiKvOverrides(azookey::host::ZenzaiTokenizerMetadata{}).empty());
}

TEST(InferenceEngineTest, BuildsPreTokenizerAndEosKvOverridesInLoadOrder) {
  azookey::host::ZenzaiTokenizerMetadata metadata;
  metadata.pre_tokenizer = "gpt2-small-japanese-char";
  metadata.eos_token_id = 2;
  metadata.vocabulary = {"[PAD]", "unused", "<s>", "unused-2", "</s>"};

  const auto overrides = azookey::host::BuildZenzaiKvOverrides(metadata);

  ASSERT_EQ(overrides.size(), 2u);
  EXPECT_EQ(overrides[0].key, "tokenizer.ggml.pre");
  EXPECT_EQ(overrides[0].type, azookey::host::ZenzaiKvOverride::Type::String);
  EXPECT_EQ(overrides[0].string_value, "gpt-2");
  EXPECT_EQ(overrides[1].key, "tokenizer.ggml.eos_token_id");
  EXPECT_EQ(overrides[1].type, azookey::host::ZenzaiKvOverride::Type::Int);
  EXPECT_EQ(overrides[1].int_value, 4);
}

TEST(InferenceEngineTest, BuildsOnlyEosKvOverrideWhenPreTokenizerIsUpstream) {
  azookey::host::ZenzaiTokenizerMetadata metadata;
  metadata.pre_tokenizer = "gpt-2";
  metadata.eos_token_id = 2;
  metadata.vocabulary = {"[PAD]", "unused", "<s>", "unused-2", "</s>"};

  const auto overrides = azookey::host::BuildZenzaiKvOverrides(metadata);

  ASSERT_EQ(overrides.size(), 1u);
  EXPECT_EQ(overrides[0].key, "tokenizer.ggml.eos_token_id");
  EXPECT_EQ(overrides[0].type, azookey::host::ZenzaiKvOverride::Type::Int);
  EXPECT_EQ(overrides[0].int_value, 4);
}

TEST(InferenceEngineTest, BuildsNoKvOverrideWhenEosIsDeclaredCorrectly) {
  azookey::host::ZenzaiTokenizerMetadata metadata;
  metadata.pre_tokenizer = "gpt-2";
  metadata.eos_token_id = 4;
  metadata.vocabulary = {"[PAD]", "unused", "<s>", "unused-2", "</s>"};

  EXPECT_TRUE(azookey::host::BuildZenzaiKvOverrides(metadata).empty());
}

TEST(InferenceEngineTest, LoadModelLoadsValidGgufWithCpuBackend) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_load_valid.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTraceTest, ModelExceptionAndFallbackProduceOneInferenceSample) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const auto model_path = TempPath("azookey_trace_exception_fallback.gguf");
  const auto log_path = TempPath("azookey_trace_exception_fallback.jsonl");
  std::remove(model_path.c_str());
  std::remove(log_path.c_str());
  WriteMinimalGguf(model_path);
  azookey::logging::RuntimeLoggerOptions log_options;
  log_options.enabled = true;
  log_options.component = "host";
  log_options.output_path = log_path;
  azookey::logging::RuntimeLogger logger(log_options);
  azookey::host::InferenceEngine engine(std::make_unique<ThrowOnceConverter>(), nullptr, {},
                                        &logger);
  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  EnableMockZenzaiCandidatesForTests(options);
  ASSERT_TRUE(engine.LoadModelWithResult(options).ok);

  const azookey::host::InferenceTelemetry telemetry{42, {}, "018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2"};
  const auto candidates =
      engine.QueryCandidates("にほん", "", kNowBase, nullptr, 10, false, &telemetry);
  ASSERT_FALSE(candidates.empty());

  std::ifstream stream(log_path);
  int inference_samples = 0;
  for (std::string line; std::getline(stream, line);) {
    if (line.find("\"phase\":\"model_inference\"") == std::string::npos) continue;
    ++inference_samples;
    EXPECT_NE(line.find("\"result\":\"ok\""), std::string::npos);
    EXPECT_NE(line.find("\"backend\":\"cpu\""), std::string::npos);
    EXPECT_EQ(line.find("\"engine\""), std::string::npos);
  }
  EXPECT_EQ(inference_samples, 1);
  std::remove(model_path.c_str());
  std::remove(log_path.c_str());
}

TEST(InferenceEngineTraceTest, RerankerCancellationLogsCancelledPhase) {
  const auto log_path = TempPath("azookey_host_rerank_cancel.jsonl");
  std::remove(log_path.c_str());
  std::atomic<bool> cancel{false};
  CancelOnScoreLearningStore store(cancel);
  azookey::logging::RuntimeLoggerOptions log_options;
  log_options.enabled = true;
  log_options.component = "host";
  log_options.output_path = log_path;
  azookey::logging::RuntimeLogger logger(log_options);
  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), &store,
                                        {}, &logger);
  const azookey::host::InferenceTelemetry telemetry{42, {}, "018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2"};

  const auto candidates =
      engine.QueryCandidates("にほん", "", kNowBase, &cancel, 10, false, &telemetry);
  EXPECT_TRUE(cancel.load(std::memory_order_relaxed));
  EXPECT_TRUE(candidates.empty());

  std::ifstream stream(log_path);
  ASSERT_TRUE(stream.is_open());
  int rerank_samples = 0;
  for (std::string line; std::getline(stream, line);) {
    if (line.find("\"phase\":\"rerank\"") == std::string::npos) continue;
    ++rerank_samples;
    EXPECT_NE(line.find("\"result\":\"cancelled\""), std::string::npos);
  }
  EXPECT_EQ(rerank_samples, 1);
  std::remove(log_path.c_str());
}

TEST(InferenceEngineTest, LoadedZenzaiRuntimeWithoutMockCandidatesFallsBackOnly) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_no_mock.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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
  EXPECT_EQ(engine->health_state(), azookey::host::HealthState::Healthy);

  std::remove(model_path.c_str());
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, LoadedZenzaiRuntimeDegradesToFallbackAndRecovers) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_degraded.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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
  EXPECT_EQ(engine->health_state(), azookey::host::HealthState::Healthy);

  auto nll_config = engine->config();
  nll_config.nll.enabled = true;
  engine->ApplyConfig(nll_config);
  const auto nll_degraded = engine->QueryCandidates("にほん", "", kNowBase);
  ASSERT_EQ(nll_degraded.size(), degraded.size());
  for (size_t i = 0; i < degraded.size(); ++i) {
    EXPECT_EQ(nll_degraded[i].surface, degraded[i].surface);
    EXPECT_EQ(nll_degraded[i].score, degraded[i].score);
    EXPECT_EQ(nll_degraded[i].debug_info, degraded[i].debug_info);
  }
  // NLL must not replace the conversion failure with its own inference error.
  ASSERT_TRUE(engine->effective_last_error().has_value());
  EXPECT_NE(engine->effective_last_error()->find("empty-generation"), std::string::npos);
  nll_config.nll.enabled = false;
  engine->ApplyConfig(nll_config);

  auto recovered = engine->QueryCandidates("にほんご", "", kNowBase);
  ASSERT_FALSE(recovered.empty());
  EXPECT_EQ(recovered.front().surface, "日本語");
  EXPECT_FALSE(engine->effective_last_error().has_value());
  EXPECT_EQ(engine->health_state(), azookey::host::HealthState::Healthy);

  // A mock runtime is loaded in the no-llama build, but cannot score NLL.
  // Repeated NLL requests must neither contaminate health nor change candidates.
  azookey::host::ZenzaiRuntimeOptions runtime_options;
  runtime_options.mock_candidates_for_tests = true;
  auto loaded = azookey::host::LoadZenzaiGgufModel(model_path, runtime_options);
  ASSERT_TRUE(loaded.ok);
  azookey::host::ZenzaiModelConverter scorer(std::move(loaded), nullptr);
  ASSERT_TRUE(scorer.runtime_loaded());
  std::vector<azookey::core::Candidate> targets(1);
  targets.front().surface = "日本語";
  targets.front().source = azookey::core::CandidateSource::SystemDictionary;
  targets.front().score = 1.0;
  nll_config.nll.enabled = true;
  engine->ApplyConfig(nll_config);
  for (int request = 0; request < 4; ++request) {
    EXPECT_EQ(scorer.RerankNll("にほんご", targets, {}, nll_config.nll).reason, "model_not_loaded");
    EXPECT_FALSE(scorer.last_error().has_value());
    EXPECT_DOUBLE_EQ(targets.front().score, 1.0);
    EXPECT_TRUE(targets.front().debug_info.empty());
    const auto unchanged = engine->QueryCandidates("にほんご", "", kNowBase);
    ASSERT_EQ(unchanged.size(), recovered.size());
    for (size_t i = 0; i < recovered.size(); ++i) {
      EXPECT_EQ(unchanged[i].surface, recovered[i].surface);
      EXPECT_EQ(unchanged[i].score, recovered[i].score);
      EXPECT_EQ(unchanged[i].debug_info, recovered[i].debug_info);
    }
    EXPECT_FALSE(engine->effective_last_error().has_value());
  }

  std::remove(model_path.c_str());
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, LiveZenzaiRequestsUseTopOneDecodeLimit) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_live_top_one.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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

  const auto fast = engine->QueryLiveConversion("にほんご", "", kNowBase + 1, nullptr);
  ASSERT_TRUE(fast);
  EXPECT_EQ(fast->surface, "にほんご");
  EXPECT_EQ(fast->source, azookey::core::CandidateSource::Heuristic);

  auto top_one = engine->QueryCandidates("にほんご", "", kNowBase + 2, nullptr, 1, false);
  ASSERT_EQ(top_one.size(), 1u);
  EXPECT_EQ(top_one.front().surface, "日本語");
  EXPECT_EQ(top_one.front().source, azookey::core::CandidateSource::Model);

  std::remove(model_path.c_str());
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, CanceledZenzaiConvertPreservesDegradedHealth) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_cancel_health.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, LoadedZenzaiRuntimeRejectsInvalidUtf8Surface) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_invalid_utf8.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, DeadlineBestSoFarTrimsOnlyIncompleteUtf8Suffix) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_deadline_utf8.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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
  const auto decode_stats = engine->last_zenzai_decode_stats();
  ASSERT_TRUE(decode_stats.has_value());
  EXPECT_TRUE(decode_stats->deadline_exceeded);

  auto invalid = engine->QueryCandidates("ないぶむこう", "", kNowBase + 1);
  ASSERT_FALSE(invalid.empty());
  EXPECT_EQ(invalid.front().surface, "ないぶむこう");
  EXPECT_NE(invalid.front().debug_info.find("invalid-utf8-surface"), std::string::npos);
  ASSERT_TRUE(engine->effective_last_error().has_value());
  EXPECT_NE(engine->effective_last_error()->find("invalid-utf8-surface"), std::string::npos);

  std::remove(model_path.c_str());
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, DeadlineCutBeamRanksBehindCompletedZenzaiCandidate) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_unfinished_rank.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_unfinished_rank_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  EnableMockZenzaiCandidatesForTests(options);
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);

  const auto candidates = engine->QueryCandidates("うちきり", "", kNowBase, nullptr, 10, false);
  const auto find = [&](const std::string& surface) {
    return std::find_if(candidates.begin(), candidates.end(),
                        [&](const auto& c) { return c.surface == surface; });
  };
  const auto completed = find("協議する");
  const auto cut = find("協議す");
  ASSERT_NE(completed, candidates.end());
  ASSERT_NE(cut, candidates.end());
  EXPECT_LT(completed, cut);
  EXPECT_LE(cut->score, completed->score);
  EXPECT_NE(cut->debug_info.find("unfinished"), std::string::npos);
  EXPECT_EQ(completed->debug_info.find("unfinished"), std::string::npos);

  std::remove(model_path.c_str());
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, UnfinishedCandidatesFollowEveryCompletedCandidate) {
  std::vector<azookey::core::Candidate> candidates(4);
  candidates[0].surface = "協議す";
  candidates[0].score = 1.39;
  candidates[1].surface = "協議する";
  candidates[1].score = 1.20;
  candidates[2].surface = "競技する";
  candidates[2].score = 0.90;
  candidates[3].surface = "協議し";
  candidates[3].score = 0.50;

  const auto ranked =
      azookey::host::RankUnfinishedBehindCompleted(candidates, {true, false, false, true});

  ASSERT_EQ(ranked.size(), 4u);
  EXPECT_EQ(ranked[0].surface, "協議する");
  EXPECT_EQ(ranked[1].surface, "競技する");
  EXPECT_EQ(ranked[2].surface, "協議す");
  EXPECT_DOUBLE_EQ(ranked[2].score, 0.90);
  EXPECT_NE(ranked[2].debug_info.find("unfinished"), std::string::npos);
  EXPECT_EQ(ranked[3].surface, "協議し");
  EXPECT_DOUBLE_EQ(ranked[3].score, 0.50);
  EXPECT_TRUE(ranked[0].debug_info.empty());
}

TEST(InferenceEngineTest, UnfinishedCandidatesKeepScoresWithoutCompletedCandidate) {
  std::vector<azookey::core::Candidate> candidates(1);
  candidates[0].surface = "日本語";
  candidates[0].score = 1.2;

  const auto ranked = azookey::host::RankUnfinishedBehindCompleted(candidates, {true});

  ASSERT_EQ(ranked.size(), 1u);
  EXPECT_DOUBLE_EQ(ranked[0].score, 1.2);
  EXPECT_THROW(azookey::host::RankUnfinishedBehindCompleted(candidates, {}), std::invalid_argument);
}

#ifdef AZOOKEY_AI_TEST_MODEL
// Exercises the beam loop wiring (end-of-sequence cost and stop condition) that the pure
// function tests above cannot reach. Runs only when a real Zenzai model is configured.
TEST(InferenceEngineTest, RealZenzaiNBestKeepsReadingTail) {
  const char* lpath = "azookey_host_engine_zenzai_real_tail.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);
  azookey::host::ModelLoadOptions options;
  options.path = AZOOKEY_AI_TEST_MODEL;
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);

  const auto candidates = engine->QueryCandidates("けいやくこうしんのじょうけんをきょうぎする", "",
                                                  kNowBase, nullptr, 5, false);
  ASSERT_FALSE(candidates.empty());
  EXPECT_EQ(candidates.front().surface, "契約更新の条件を協議する");

  RemoveProtectedStoreFile(lpath);
}
#endif

TEST(InferenceEngineTest, RecommendsAvailableZenzaiThreadsCappedAtEight) {
  EXPECT_EQ(azookey::host::RecommendedZenzaiThreadCount(0), 1);
  EXPECT_EQ(azookey::host::RecommendedZenzaiThreadCount(2), 2);
  EXPECT_EQ(azookey::host::RecommendedZenzaiThreadCount(6), 6);
  EXPECT_EQ(azookey::host::RecommendedZenzaiThreadCount(8), 8);
  EXPECT_EQ(azookey::host::RecommendedZenzaiThreadCount(20), 8);
}

TEST(InferenceEngineTest, FindsCommonPromptTokenPrefix) {
  EXPECT_EQ(azookey::host::CommonPrefixLength({}, {}), 0u);
  EXPECT_EQ(azookey::host::CommonPrefixLength({1, 2, 3}, {1, 2, 4}), 2u);
  EXPECT_EQ(azookey::host::CommonPrefixLength({1, 2}, {1, 2, 3}), 2u);
  EXPECT_EQ(azookey::host::CommonPrefixLength({1, 2, 3}, {1}), 1u);
  EXPECT_EQ(azookey::host::CommonPrefixLength({1}, {2}), 0u);
}

TEST(InferenceEngineTest, RetainsPromptPrefixOnlyWhenLogitsCanBeServed) {
  // Without cached logits the final prompt token is re-decoded, so its logits are observable.
  EXPECT_EQ(azookey::host::RetainedPromptPrefixLength({1, 2, 3}, {1, 2, 3}, false), 2u);
  EXPECT_EQ(azookey::host::RetainedPromptPrefixLength({1, 2, 3}, {1, 2, 3}, true), 3u);
  // A prompt that only extends or diverges from the cache never reuses its last token.
  EXPECT_EQ(azookey::host::RetainedPromptPrefixLength({1, 2}, {1, 2, 3}, true), 2u);
  EXPECT_EQ(azookey::host::RetainedPromptPrefixLength({1, 2, 3}, {1, 2}, true), 1u);
  EXPECT_EQ(azookey::host::RetainedPromptPrefixLength({1, 2, 3}, {1, 2, 4}, true), 2u);
  EXPECT_EQ(azookey::host::RetainedPromptPrefixLength({1}, {2}, true), 0u);
  EXPECT_EQ(azookey::host::RetainedPromptPrefixLength({}, {1, 2}, true), 0u);
  EXPECT_EQ(azookey::host::RetainedPromptPrefixLength({1, 2}, {}, true), 0u);
  EXPECT_EQ(azookey::host::RetainedPromptPrefixLength({1}, {1}, true), 1u);
  EXPECT_EQ(azookey::host::RetainedPromptPrefixLength({1}, {1}, false), 0u);
}

TEST(InferenceEngineTest, PlansInitialBeamSequencesFromPromptRoot) {
  const auto plan = azookey::host::PlanBeamSequenceAssignments({0, 0, 0, 0}, {});

  EXPECT_EQ(plan.assignments, (std::vector<int32_t>{1, 2, 3, 4}));
  EXPECT_TRUE(plan.releases.empty());
  EXPECT_EQ(plan.copies,
            (std::vector<azookey::host::BeamSequenceCopy>{{0, 1}, {0, 2}, {0, 3}, {0, 4}}));
}

TEST(InferenceEngineTest, PlansBeamSequenceRetentionReleaseAndCopiesDeterministically) {
  const auto plan = azookey::host::PlanBeamSequenceAssignments({1, 1, 2, 2}, {1, 2, 3, 4});

  EXPECT_EQ(plan.assignments, (std::vector<int32_t>{1, 3, 2, 4}));
  EXPECT_EQ(plan.releases, (std::vector<int32_t>{3, 4}));
  EXPECT_EQ(plan.copies, (std::vector<azookey::host::BeamSequenceCopy>{{1, 3}, {2, 4}}));
}

TEST(InferenceEngineTest, PlansPrunedBeamSequencesWithoutUnnecessaryCopies) {
  const auto plan = azookey::host::PlanBeamSequenceAssignments({2, 1}, {1, 2, 3, 4});

  EXPECT_EQ(plan.assignments, (std::vector<int32_t>{2, 1}));
  EXPECT_EQ(plan.releases, (std::vector<int32_t>{3, 4}));
  EXPECT_TRUE(plan.copies.empty());
}

TEST(InferenceEngineTest,
     CompletedBeamPaysForEndOfSequenceSoTruncatedReadingRanksBelowFullReading) {
  using azookey::host::BeamRankScore;
  using azookey::host::CompletedBeamScore;
  // 「…協議す」 ends one character early: its prefix is confident but the model does not
  // expect end-of-sequence after it. 「…協議する」 ends where the model expects it to.
  const auto truncated = CompletedBeamScore(-0.0034, 11, -6.0);
  const auto full = CompletedBeamScore(-0.0040, 12, -0.001);

  EXPECT_DOUBLE_EQ(truncated.total_logprob, -6.0034);
  EXPECT_EQ(truncated.token_count, 12);
  EXPECT_GT(BeamRankScore(full.total_logprob, full.token_count),
            BeamRankScore(truncated.total_logprob, truncated.token_count));
  // Without the end-of-sequence cost the truncated prefix would rank first.
  EXPECT_GT(BeamRankScore(-0.0034, 11), BeamRankScore(-0.0040, 12));
  EXPECT_EQ(BeamRankScore(0.0, 0), -std::numeric_limits<double>::infinity());
}

TEST(InferenceEngineTest, BeamSearchKeepsGoingWhileActiveBeamOutranksCompletedQuota) {
  using azookey::host::ShouldStopBeamSearch;
  // Four truncated hypotheses completed in the same step; the full reading is still active.
  EXPECT_FALSE(ShouldStopBeamSearch({-0.50, -0.52, -0.55, -0.60}, {-0.0003, -0.9}, 4));
  EXPECT_FALSE(ShouldStopBeamSearch({-0.01, -0.02, -0.03}, {-5.0}, 4));
  EXPECT_FALSE(ShouldStopBeamSearch({-0.01}, {}, 0));
}

TEST(InferenceEngineTest, BeamSearchStopsWhenNoActiveBeamOutranksWeakestKeptCompletion) {
  using azookey::host::ShouldStopBeamSearch;
  EXPECT_TRUE(ShouldStopBeamSearch({-0.0004, -0.50, -0.52, -0.55}, {-0.60, -0.9}, 4));
  EXPECT_TRUE(ShouldStopBeamSearch({-0.10}, {}, 1));
  // Only the best `candidate_limit` completions are kept, so the threshold is the 4th best.
  EXPECT_FALSE(ShouldStopBeamSearch({-5.0, -0.1, -0.2, -0.3, -0.4}, {-0.35}, 4));
  EXPECT_TRUE(ShouldStopBeamSearch({-5.0, -0.1, -0.2, -0.3, -0.4}, {-0.45}, 4));
}

TEST(InferenceEngineTest, ModelConversionDeadlineUsesSixHundredMillisecondBudget) {
  const char* lpath = "azookey_host_engine_model_budget.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());

  auto converter = std::make_unique<DeadlineCapturingConverter>();
  auto* converter_ptr = converter.get();
  azookey::host::EngineConfig config;
  azookey::host::InferenceEngine engine(std::move(converter), &store, config);

  const auto candidates = engine.QueryCandidates("にほんご", "", kNowBase);

  ASSERT_FALSE(candidates.empty());
  EXPECT_EQ(candidates.front().source, azookey::core::CandidateSource::Model);
  EXPECT_TRUE(converter_ptr->saw_deadline());
  EXPECT_LE(converter_ptr->remaining_budget(), std::chrono::milliseconds(600));
  EXPECT_GE(converter_ptr->remaining_budget(), std::chrono::milliseconds(500));

  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, ZenzaiCandidateLimitCountsOnlySaneUniqueCandidates) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_zenzai_sane_unique.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, LoadModelRejectsInvalidGguf) {
  const char* lpath = "azookey_host_engine_load_invalid.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, VulkanFailureFallsBackToCpuAndRetainsHealthError) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture only exercises the mock control path.";
  }
  const char* learning_path = "azookey_host_engine_vulkan_fallback.tsv";
  std::remove(learning_path);
  azookey::learning::LearningStore store(learning_path, &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);
  const auto model_path = TempPath("azookey_vulkan_fallback.gguf");
  WriteMinimalGguf(model_path);
  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  options.backend = azookey::host::BackendKind::Vulkan;
  const auto result = engine->LoadModelWithResult(options);
  ASSERT_TRUE(result.ok);
  ASSERT_TRUE(result.error);
  EXPECT_NE(result.error->find("Vulkan"), std::string::npos);
  EXPECT_EQ(engine->backend(), azookey::host::BackendKind::Cpu);
  EXPECT_TRUE(engine->model_loaded());
  EXPECT_EQ(engine->health_snapshot().last_error, result.error);

  // A later successful explicit CPU load recovers Health.
  options.backend = azookey::host::BackendKind::Cpu;
  EXPECT_TRUE(engine->LoadModelWithResult(options).ok);
  EXPECT_FALSE(engine->health_snapshot().last_error);
  std::remove(model_path.c_str());
  std::remove(learning_path);
}

TEST(InferenceEngineTest, FailedVulkanAndCpuLoadsReturnFixedError) {
  const char* learning_path = "azookey_host_engine_both_backends_fail.tsv";
  std::remove(learning_path);
  azookey::learning::LearningStore store(learning_path, &azookey::learning::test::Crypto());
  const auto model_path = TempPath("azookey_both_backends_fail.gguf");
  WriteMinimalGguf(model_path);
  for (const bool unknown_exception : {false, true}) {
    auto engine = MakeEngine(store);
    azookey::host::ModelLoadOptions options;
    options.path = model_path;
    options.backend = azookey::host::BackendKind::Vulkan;
    std::vector<azookey::host::BackendKind> attempts;
    options.before_load_for_tests = [&](azookey::host::BackendKind backend) {
      attempts.push_back(backend);
      if (backend == azookey::host::BackendKind::Vulkan) {
        if (unknown_exception) {
          throw 42;
        }
        throw std::runtime_error("Vulkan test failure");
      }
      throw std::runtime_error("CPU test failure");
    };
    const auto result = engine->LoadModelWithResult(options);
    EXPECT_FALSE(result.ok);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(*result.error, "Vulkan and CPU model load failed");
    EXPECT_EQ(result.error->find("Vulkan test failure"), std::string::npos);
    EXPECT_EQ(result.error->find("CPU test failure"), std::string::npos);
    EXPECT_EQ(attempts, (std::vector<azookey::host::BackendKind>{azookey::host::BackendKind::Vulkan,
                                                                 azookey::host::BackendKind::Cpu}));
    EXPECT_FALSE(engine->model_loaded());
    EXPECT_EQ(engine->backend(), azookey::host::BackendKind::Cpu);
    EXPECT_EQ(engine->health_snapshot().last_error, result.error);
  }
  std::remove(model_path.c_str());
  std::remove(learning_path);
}

TEST(InferenceEngineTest, LoadModelCudaFallsBackToCpuForNow) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_load_cuda.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, LoadModelFailureKeepsPreviouslyLoadedModel) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_reload_failure.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, LoadModelSuccessDoesNotChainWrappers) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_reload_success.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, UserDictionaryDuplicateKeepsUserSourceOverZenzaiModel) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const char* lpath = "azookey_host_engine_user_dict_zenzai_dedup.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);

  const std::string udict_path =
      (std::filesystem::temp_directory_path() / "azookey_host_engine_user_zenzai.json").string();
  RemoveProtectedStoreFile(udict_path);
  azookey::learning::UserDictionary dict(udict_path, &azookey::learning::test::Crypto());
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
  RemoveProtectedStoreFile(udict_path);
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, LoadModelStateAccessorsThreadedSmoke) {
  const char* lpath = "azookey_host_engine_load_threaded.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
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

  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, QueryCandidatesKeepsHealthAndModelSwapResponsive) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  using namespace std::chrono_literals;

  const char* lpath = "azookey_host_engine_query_load_responsive.tsv";
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());

  auto converter = std::make_unique<BlockingConverter>();
  auto* blocking_converter = converter.get();
  azookey::host::EngineConfig cfg;
  azookey::host::InferenceEngine engine(std::move(converter), &store, cfg);

  const std::string model_path = TempPath("azookey_query_load_responsive.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  std::vector<azookey::core::Candidate> query_result;
  std::thread query_thread(
      [&]() { query_result = engine.QueryCandidates("にほん", "", kNowBase); });

  const bool query_entered = blocking_converter->WaitUntilEntered(1s);
  EXPECT_TRUE(query_entered);

  auto health_future = std::async(std::launch::async, [&]() { return engine.health_snapshot(); });
  const bool health_ready = health_future.wait_for(100ms) == std::future_status::ready;
  EXPECT_TRUE(health_ready) << "Health must not wait for a long converter call";
  if (health_ready) {
    EXPECT_FALSE(health_future.get().model_loaded);
  }

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
  const bool load_ready = load_future.wait_for(1s) == std::future_status::ready;
  EXPECT_TRUE(load_ready) << "Model replacement must not wait for the old converter call";

  blocking_converter->Release();
  query_thread.join();

  const auto load_result = load_future.get();
  EXPECT_TRUE(load_result.ok);
  EXPECT_TRUE(engine.model_loaded());
  ASSERT_FALSE(query_result.empty());
  EXPECT_EQ(query_result.front().debug_info, "blocking-converter");

  std::remove(model_path.c_str());
  RemoveProtectedStoreFile(lpath);
}

// ---- M35: typo correction ----

namespace {

// A converter with a real lexicon, so Contains() can tell a dictionary entry
// from a word the engine has only seen committed.
std::unique_ptr<azookey::core::SimpleConverter> MakeConverterWithDictionary(
    const std::vector<std::pair<std::string, std::string>>& entries) {
  ScopedTempDirectory temp;
  const auto path = temp.File("dictionary.tsv");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (const auto& entry : entries) {
      out << entry.first << '\t' << entry.second << "\t2.0\n";
    }
    out.close();
    if (!out) {
      ADD_FAILURE() << "Could not write engine test dictionary: " << path;
      return std::make_unique<azookey::core::SimpleConverter>();
    }
  }
  auto converter = std::make_unique<azookey::core::SimpleConverter>();
  EXPECT_TRUE(converter->LoadFromTsv(path)) << "Could not load engine test dictionary: " << path;
  return converter;
}

std::unique_ptr<azookey::host::InferenceEngine> MakeEngineWithConverter(
    std::unique_ptr<azookey::core::IConverter> converter, azookey::learning::LearningStore& store,
    azookey::host::EngineConfig cfg) {
  return std::make_unique<azookey::host::InferenceEngine>(std::move(converter), &store, cfg);
}

bool HasCandidateMarked(const std::vector<azookey::core::Candidate>& candidates,
                        const std::string& mark) {
  return std::any_of(candidates.begin(), candidates.end(),
                     [&](const auto& c) { return c.debug_info == mark; });
}

}  // namespace

TEST(EngineTypoCorrectionTest, SuggestInjectsAMarkedCandidateOnlyOverTheThreshold) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::TypoCorrectionStore typo(temp.File("typo.tsv"),
                                              &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.typo_correction_mode = "suggest";
  cfg.typo_min_count = 3;
  auto engine =
      MakeEngineWithConverter(MakeConverterWithDictionary({{"こんにちは", "今日は"}}), store, cfg);
  engine->SetTypoStore(&typo);

  // Below the threshold the pair is accumulated but never applied.
  EXPECT_TRUE(engine->ObserveTypo("こんちには", "こんにちは", kNowBase));
  EXPECT_TRUE(engine->ObserveTypo("こんちには", "こんにちは", kNowBase));
  auto below = engine->QueryCandidatesEx("こんちには", "", kNowBase, nullptr);
  EXPECT_FALSE(HasCandidateMarked(below.candidates, "typo-correction"));

  EXPECT_TRUE(engine->ObserveTypo("こんちには", "こんにちは", kNowBase));
  auto applied = engine->QueryCandidatesEx("こんちには", "", kNowBase, nullptr);
  EXPECT_TRUE(HasCandidateMarked(applied.candidates, "typo-correction"));
  // suggest leaves the preedit alone: only auto_replace reports a substitution.
  EXPECT_TRUE(applied.corrected_reading.empty());
  // The marked candidate leads, and it carries the corrected reading.
  ASSERT_FALSE(applied.candidates.empty());
  EXPECT_EQ(applied.candidates.front().debug_info, "typo-correction");
  EXPECT_EQ(applied.candidates.front().reading, "こんにちは");
}

TEST(EngineTypoCorrectionTest, AutoReplaceConvertsTheCorrectedReadingAndReportsIt) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::TypoCorrectionStore typo(temp.File("typo.tsv"),
                                              &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.typo_correction_mode = "auto_replace";
  cfg.typo_min_count = 2;
  auto engine =
      MakeEngineWithConverter(MakeConverterWithDictionary({{"こんにちは", "今日は"}}), store, cfg);
  engine->SetTypoStore(&typo);

  engine->ObserveTypo("こんちには", "こんにちは", kNowBase);
  engine->ObserveTypo("こんちには", "こんにちは", kNowBase);

  auto result = engine->QueryCandidatesEx("こんちには", "", kNowBase, nullptr);
  EXPECT_EQ(result.corrected_reading, "こんにちは");
  // The whole pipeline ran on the corrected reading, so the dictionary entry
  // for it is present.
  EXPECT_TRUE(std::any_of(result.candidates.begin(), result.candidates.end(),
                          [](const auto& c) { return c.surface == "今日は"; }));
}

TEST(EngineTypoCorrectionTest, OffInjectsNothingAndLearnsNothing) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::TypoCorrectionStore typo(temp.File("typo.tsv"),
                                              &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.typo_correction_mode = "off";
  cfg.typo_min_count = 1;
  auto engine =
      MakeEngineWithConverter(MakeConverterWithDictionary({{"こんにちは", "今日は"}}), store, cfg);
  engine->SetTypoStore(&typo);

  // "off" gates the learning as well as the application.
  EXPECT_FALSE(engine->ObserveTypo("こんちには", "こんにちは", kNowBase));
  EXPECT_EQ(typo.size(), 0u);

  // Even with a table populated behind the engine's back, off injects nothing.
  typo.Observe("こんちには", "こんにちは", kNowBase);
  auto result = engine->QueryCandidatesEx("こんちには", "", kNowBase, nullptr);
  EXPECT_FALSE(HasCandidateMarked(result.candidates, "typo-correction"));
  EXPECT_TRUE(result.corrected_reading.empty());
}

TEST(EngineTypoCorrectionTest, WithoutAStoreQueryIsUnchanged) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::host::EngineConfig cfg;
  cfg.typo_correction_mode = "suggest";
  auto engine = MakeEngine(store, cfg);

  auto result = engine->QueryCandidatesEx("こんちには", "", kNowBase, nullptr);
  EXPECT_TRUE(result.corrected_reading.empty());
  EXPECT_FALSE(HasCandidateMarked(result.candidates, "typo-correction"));
}

// ---- M36-A: new word mining ----

TEST(EngineAutoWordMiningTest, RepeatedCommitsOfAnUnknownWordAccumulateAsPending) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::AutoWordStore auto_words(temp.File("auto_words.tsv"),
                                              &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.auto_word_mining_enabled = true;
  cfg.auto_word_auto_register = false;
  cfg.auto_word_min_count = 3;
  auto engine =
      MakeEngineWithConverter(MakeConverterWithDictionary({{"きしゃ", "汽車"}}), store, cfg);
  engine->SetAutoWordStore(&auto_words);

  // Commit() feeds the converter's own Learn(), so without the
  // dictionary-vs-learned distinction the second commit would look "known" and
  // the count would stop at one.
  for (int i = 0; i < 3; ++i) {
    engine->CommitObservation("あずきー", "アズーキー", kNowBase + static_cast<uint64_t>(i));
  }
  const auto pending = auto_words.ListByState(azookey::learning::AutoWordState::Pending);
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0].surface, "アズーキー");
  EXPECT_EQ(pending[0].reading, "あずきー");
  EXPECT_EQ(pending[0].count, 3u);
  // registrationMode "confirm": reaching the threshold does not confirm it.
  EXPECT_EQ(auto_words.ListByState(azookey::learning::AutoWordState::Confirmed).size(), 0u);
}

TEST(EngineAutoWordMiningTest, AutoModeConfirmsAtTheThreshold) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::AutoWordStore auto_words(temp.File("auto_words.tsv"),
                                              &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.auto_word_mining_enabled = true;
  cfg.auto_word_auto_register = true;
  cfg.auto_word_min_count = 2;
  auto engine = MakeEngine(store, cfg);
  engine->SetAutoWordStore(&auto_words);

  engine->CommitObservation("あずきー", "アズーキー", kNowBase);
  EXPECT_EQ(auto_words.ListByState(azookey::learning::AutoWordState::Confirmed).size(), 0u);
  engine->CommitObservation("あずきー", "アズーキー", kNowBase + 1);
  EXPECT_EQ(auto_words.ListByState(azookey::learning::AutoWordState::Confirmed).size(), 1u);
}

TEST(EngineAutoWordMiningTest, KnownWordsAndNoisyShapesAreNotMined) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::AutoWordStore auto_words(temp.File("auto_words.tsv"),
                                              &azookey::learning::test::Crypto());
  azookey::learning::UserDictionary user_dict(temp.File("userdict.json"),
                                              &azookey::learning::test::Crypto());
  azookey::learning::UserWord registered;
  registered.word = "azooKey社";
  registered.ruby = "あずきーしゃ";
  user_dict.Add(registered);

  azookey::host::EngineConfig cfg;
  cfg.auto_word_mining_enabled = true;
  auto engine =
      MakeEngineWithConverter(MakeConverterWithDictionary({{"きしゃ", "汽車"}}), store, cfg);
  engine->SetUserDictionary(&user_dict);
  engine->SetAutoWordStore(&auto_words);

  engine->CommitObservation("きしゃ", "汽車", kNowBase);             // in the dictionary
  engine->CommitObservation("あずきーしゃ", "azooKey社", kNowBase);  // in the user dictionary
  engine->CommitObservation("あ", "亜", kNowBase);                   // one code point
  engine->CommitObservation("", "なぞ", kNowBase);                   // no reading
  engine->CommitObservation("あいう", "あいう", kNowBase);           // nothing converted
  engine->CommitObservation("にせんにじゅう", "2020年", kNowBase);   // contains digits
  engine->CommitObservation("ゆーあーるえる", "http", kNowBase);     // ASCII only
  engine->CommitObservation("かっこ", "「」", kNowBase);             // punctuation only
  engine->CommitObservation("abc", "エービーシー", kNowBase);        // reading is not kana

  EXPECT_EQ(auto_words.Size(), 0u);
}

TEST(EngineAutoWordMiningTest, MiningDisabledAndNoStoreAreBothNoOps) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::AutoWordStore auto_words(temp.File("auto_words.tsv"),
                                              &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.auto_word_mining_enabled = false;
  auto engine = MakeEngine(store, cfg);
  engine->SetAutoWordStore(&auto_words);
  engine->CommitObservation("あずきー", "アズーキー", kNowBase);
  EXPECT_EQ(auto_words.Size(), 0u);

  // No store at all must not crash the commit path.
  azookey::host::EngineConfig enabled;
  enabled.auto_word_mining_enabled = true;
  auto storeless = MakeEngine(store, enabled);
  EXPECT_TRUE(storeless->CommitObservation("あずきー", "アズーキー", kNowBase));
}

TEST(EngineAutoWordMiningTest, DuplicateObservationIdIsNotMinedTwice) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::AutoWordStore auto_words(temp.File("auto_words.tsv"),
                                              &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.auto_word_mining_enabled = true;
  auto engine = MakeEngine(store, cfg);
  engine->SetAutoWordStore(&auto_words);

  EXPECT_TRUE(engine->CommitObservation("あずきー", "アズーキー", kNowBase, "obs-1"));
  // A resend after a pipe drop must not inflate the mining count either.
  EXPECT_FALSE(engine->CommitObservation("あずきー", "アズーキー", kNowBase, "obs-1"));
  const auto pending = auto_words.ListByState(azookey::learning::AutoWordState::Pending);
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0].count, 1u);
}

TEST(EngineAutoWordMiningTest, ModelBackendStillConsultsTheFallbackLexicon) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::AutoWordStore auto_words(temp.File("auto_words.tsv"),
                                              &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.auto_word_mining_enabled = true;
  auto engine =
      MakeEngineWithConverter(MakeConverterWithDictionary({{"きしゃ", "汽車"}}), store, cfg);
  engine->SetAutoWordStore(&auto_words);

  const std::filesystem::path model_file = temp.File("zenzai.gguf");
  WriteMinimalGguf(model_file);
  azookey::host::ModelLoadOptions options;
  options.path = azookey::core::PathToUtf8(model_file);
  options.backend = azookey::host::BackendKind::Cpu;
  EnableMockZenzaiCandidatesForTests(options);
  const auto loaded = engine->LoadModelWithResult(options);
  ASSERT_TRUE(loaded.ok) << loaded.error.value_or("");

  // With Zenzai active, a word the fallback lexicon knows is still known.
  engine->CommitObservation("きしゃ", "汽車", kNowBase);
  EXPECT_EQ(auto_words.Size(), 0u);
  engine->CommitObservation("あずきー", "アズーキー", kNowBase);
  EXPECT_EQ(auto_words.ListByState(azookey::learning::AutoWordState::Pending).size(), 1u);
}

// ---- M36-A: injection of confirmed words (spec section 6) ----

namespace {

const azookey::core::Candidate* FindAutoWord(
    const std::vector<azookey::core::Candidate>& candidates, const std::string& surface) {
  const auto it = std::find_if(candidates.begin(), candidates.end(), [&](const auto& c) {
    return c.surface == surface && c.debug_info.find("auto-word") != std::string::npos;
  });
  return it == candidates.end() ? nullptr : &*it;
}

}  // namespace

TEST(EngineAutoWordInjectionTest, ConfirmedWordsAreInjectedAndPendingWordsAreNot) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("azookey_engine_inject_learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::AutoWordStore auto_words(temp.File("azookey_engine_inject.tsv"),
                                              &azookey::learning::test::Crypto());
  auto_words.Observe("阿頭季", "あずき", kNowBase, 3, false);
  auto_words.Observe("小豆期", "あずき", kNowBase, 3, false);
  ASSERT_TRUE(auto_words.Confirm("阿頭季", "あずき"));

  azookey::host::EngineConfig cfg;
  cfg.auto_word_default_score = 1.25;
  auto engine = MakeEngine(store, cfg);
  engine->SetAutoWordStore(&auto_words);

  const auto candidates = engine->QueryCandidates("あずき", "", kNowBase);
  const auto* confirmed = FindAutoWord(candidates, "阿頭季");
  ASSERT_NE(confirmed, nullptr);
  EXPECT_EQ(confirmed->reading, "あずき");
  EXPECT_EQ(confirmed->source, azookey::core::CandidateSource::UserDictionary);
  // A mined word carries no score of its own, so the configured default applies.
  EXPECT_DOUBLE_EQ(confirmed->score, 1.25);
  EXPECT_EQ(FindAutoWord(candidates, "小豆期"), nullptr);

  // Detaching the store stops the injection.
  engine->SetAutoWordStore(nullptr);
  EXPECT_EQ(FindAutoWord(engine->QueryCandidates("あずき", "", kNowBase), "阿頭季"), nullptr);
}

TEST(EngineAutoWordInjectionTest, DictionarySwitchStopsConfirmedWordsAndReenablesWithoutDeletion) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::AutoWordStore auto_words(temp.File("auto_words.tsv"),
                                              &azookey::learning::test::Crypto());
  auto_words.Observe("阿頭季", "あずき", kNowBase, 3, false);
  ASSERT_TRUE(auto_words.Confirm("阿頭季", "あずき"));
  azookey::host::EngineConfig config;
  config.dictionary.auto_words_enabled = false;
  auto engine = MakeEngine(store, config);
  engine->SetAutoWordStore(&auto_words);
  EXPECT_EQ(FindAutoWord(engine->QueryCandidates("あずき", "", kNowBase), "阿頭季"), nullptr);
  EXPECT_EQ(auto_words.LookupConfirmed("あずき").size(), 1u);
  EXPECT_TRUE(engine->config().auto_word_mining_enabled);

  config.dictionary.auto_words_enabled = true;
  engine->ApplyConfig(config);
  EXPECT_NE(FindAutoWord(engine->QueryCandidates("あずき", "", kNowBase), "阿頭季"), nullptr);
  engine->EnableDictionaryLayer(azookey::learning::LayerId::AutoWords, false);
  EXPECT_EQ(FindAutoWord(engine->QueryCandidates("あずき", "", kNowBase), "阿頭季"), nullptr);
  engine->EnableDictionaryLayer(azookey::learning::LayerId::AutoWords, true);
  EXPECT_NE(FindAutoWord(engine->QueryCandidates("あずき", "", kNowBase), "阿頭季"), nullptr);
  config.dictionary.auto_words_enabled = false;
  engine->ApplyConfig(config);
  EXPECT_EQ(FindAutoWord(engine->QueryCandidates("あずき", "", kNowBase), "阿頭季"), nullptr);
  EXPECT_EQ(auto_words.LookupConfirmed("あずき").size(), 1u);
}

TEST(EngineAutoWordInjectionTest, SettingsReloadAppliesDictionarySwitchesWithoutChangingMining) {
  ScopedTempDirectory temp;
  const auto settings_path = temp.File("settings.json");
  {
    std::ofstream out(settings_path);
    out << R"({"dictionary":{"autoWordsEnabled":false}})";
  }
  azookey::host::SettingsStore settings(settings_path);
  ASSERT_EQ(settings.Load().status, azookey::host::SettingsLoadStatus::Loaded);
  azookey::learning::LearningStore store(temp.File("learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::AutoWordStore auto_words(temp.File("auto_words.tsv"),
                                              &azookey::learning::test::Crypto());
  auto_words.Observe("阿頭季", "あずき", kNowBase, 3, false);
  ASSERT_TRUE(auto_words.Confirm("阿頭季", "あずき"));
  auto engine =
      MakeEngine(store, azookey::host::ApplyRuntimeSettingsToEngineConfig({}, settings.settings()));
  engine->SetAutoWordStore(&auto_words);
  EXPECT_EQ(FindAutoWord(engine->QueryCandidates("あずき", "", kNowBase), "阿頭季"), nullptr);
  {
    std::ofstream out(settings_path);
    out << R"({"dictionary":{"autoWordsEnabled":true}})";
  }
  ASSERT_EQ(settings.Reload().status, azookey::host::SettingsLoadStatus::Loaded);
  engine->ApplyConfig(
      azookey::host::ApplyRuntimeSettingsToEngineConfig(engine->config(), settings.settings()));
  EXPECT_NE(FindAutoWord(engine->QueryCandidates("あずき", "", kNowBase), "阿頭季"), nullptr);
  EXPECT_TRUE(engine->config().auto_word_mining_enabled);
  EXPECT_FALSE(engine->config().auto_word_auto_register);
}

TEST(EngineAutoWordInjectionTest, ConfirmModeInjectsOnlyAfterApproval) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("azookey_engine_inject_confirm_learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::AutoWordStore auto_words(temp.File("azookey_engine_inject_confirm.tsv"),
                                              &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.auto_word_mining_enabled = true;
  cfg.auto_word_auto_register = false;  // registrationMode "confirm"
  cfg.auto_word_min_count = 2;
  auto engine = MakeEngine(store, cfg);
  engine->SetAutoWordStore(&auto_words);

  for (int i = 0; i < 3; ++i) {
    engine->CommitObservation("あずきー", "アズーキー", kNowBase + static_cast<uint64_t>(i));
  }
  // Past the threshold, still pending, still not injected.
  EXPECT_EQ(auto_words.ListByState(azookey::learning::AutoWordState::Pending).size(), 1u);
  EXPECT_EQ(FindAutoWord(engine->QueryCandidates("あずきー", "", kNowBase + 10), "アズーキー"),
            nullptr);

  ASSERT_TRUE(auto_words.Confirm("アズーキー", "あずきー"));
  EXPECT_NE(FindAutoWord(engine->QueryCandidates("あずきー", "", kNowBase + 11), "アズーキー"),
            nullptr);
}

TEST(EngineAutoWordTest, TrendingIngestWaitsForFailedResetRollbackAndPreservesBothWords) {
  class FailNextEncryption final : public azookey::learning::ByteCrypto {
   public:
    mutable std::atomic<bool> fail_next{false};
    mutable std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released{release.get_future().share()};
    bool Encrypt(const std::vector<uint8_t>& plain, std::vector<uint8_t>& cipher) const override {
      if (fail_next.exchange(false)) {
        entered.set_value();
        released.wait();
        return false;
      }
      return cipher_.Encrypt(plain, cipher);
    }
    bool Decrypt(const std::vector<uint8_t>& cipher, std::vector<uint8_t>& plain) const override {
      return cipher_.Decrypt(cipher, plain);
    }

   private:
    azookey::learning::test::TestByteCrypto cipher_;
  } crypto;
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"));
  azookey::learning::TypoCorrectionStore typo_store(temp.File("typos.tsv"), &crypto);
  azookey::learning::AutoWordStore auto_words(temp.File("auto_words.tsv"), &crypto);
  auto_words.Observe("旧語", "きゅうご", kNowBase, 1, true);
  ASSERT_TRUE(auto_words.Save());
  auto engine = MakeEngine(store);
  engine->SetAutoWordStore(&auto_words);
  crypto.fail_next = true;
  auto entered = crypto.entered.get_future();
  auto reset = std::async(std::launch::async, [&] {
    return engine->ResetLearningStore(azookey::host::LearningDataStore::AutoWord);
  });
  const auto reset_entered = entered.wait_for(std::chrono::seconds(5));
  if (reset_entered != std::future_status::ready) {
    crypto.release.set_value();
    reset.wait();
    FAIL() << "Reset did not reach the encryption barrier";
  }
  const std::vector<azookey::learning::AutoWord> words{{"新語", "しんご"}};
  std::promise<void> ingest_started;
  auto started = ingest_started.get_future();
  auto ingest = std::async(std::launch::async, [&] {
    ingest_started.set_value();
    return engine->IngestTrendingWords(words, kNowBase + 1, true);
  });
  const auto ingest_entered = started.wait_for(std::chrono::seconds(5));
  const auto ingest_pending = ingest.wait_for(std::chrono::milliseconds(50));
  crypto.release.set_value();
  EXPECT_EQ(ingest_entered, std::future_status::ready);
  EXPECT_EQ(ingest_pending, std::future_status::timeout);
  EXPECT_EQ(reset.get(), azookey::host::InferenceEngine::ResetOutcome::SaveFailed);
  ASSERT_TRUE(ingest.get());
  EXPECT_EQ(auto_words.LookupConfirmed("きゅうご").size(), 1u);
  EXPECT_EQ(auto_words.LookupConfirmed("しんご").size(), 1u);
  azookey::learning::AutoWordStore reloaded(auto_words.path(), &crypto);
  ASSERT_TRUE(reloaded.Load());
  EXPECT_EQ(reloaded.SerializeText(), auto_words.SerializeText());

  // Prove the engine boundary is shared independently of AutoWordStore's mutex:
  // a typo reset holds state_mutex_ but leaves the auto-word store unlocked.
  engine->SetTypoStore(&typo_store);
  crypto.entered = std::promise<void>();
  crypto.release = std::promise<void>();
  crypto.released = crypto.release.get_future().share();
  crypto.fail_next = true;
  auto typo_entered = crypto.entered.get_future();
  auto typo_reset = std::async(std::launch::async, [&] {
    return engine->ResetLearningStore(azookey::host::LearningDataStore::Typo);
  });
  if (typo_entered.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    crypto.release.set_value();
    typo_reset.wait();
    FAIL() << "Typo reset did not reach the encryption barrier";
  }
  std::promise<void> second_started;
  auto second_entered = second_started.get_future();
  auto second_ingest = std::async(std::launch::async, [&] {
    second_started.set_value();
    return engine->IngestTrendingWords(words, kNowBase + 2, true);
  });
  const auto second_attempted = second_entered.wait_for(std::chrono::seconds(5));
  const auto second_pending = second_ingest.wait_for(std::chrono::milliseconds(100));
  crypto.release.set_value();
  EXPECT_EQ(second_attempted, std::future_status::ready);
  EXPECT_EQ(second_pending, std::future_status::timeout);
  EXPECT_EQ(typo_reset.get(), azookey::host::InferenceEngine::ResetOutcome::SaveFailed);
  ASSERT_TRUE(second_ingest.get());
  ASSERT_TRUE(reloaded.Load());
  EXPECT_EQ(reloaded.SerializeText(), auto_words.SerializeText());
}

TEST(EngineAutoWordTest, TrendingIngestRejectsUnavailableAndUnreadableStores) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"));
  const azookey::learning::test::TestByteCrypto crypto;
  azookey::learning::AutoWordStore auto_words(temp.File("auto_words.tsv"), &crypto);
  auto engine = MakeEngine(store);
  const std::vector<azookey::learning::AutoWord> words{{"新語", "しんご"}};
  EXPECT_FALSE(engine->IngestTrendingWords(words, kNowBase, true));
  const auto encrypted_path = EncryptedPathFor(auto_words.path());
  {
    std::ofstream file(encrypted_path, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    file << "unreadable encrypted data";
  }
  ASSERT_FALSE(auto_words.Load());
  ASSERT_TRUE(auto_words.save_blocked());
  engine->SetAutoWordStore(&auto_words);
  EXPECT_FALSE(engine->IngestTrendingWords(words, kNowBase, true));
  EXPECT_EQ(auto_words.Size(), 0u);
  std::ifstream file(encrypted_path, std::ios::binary);
  std::string preserved;
  std::getline(file, preserved);
  EXPECT_EQ(preserved, "unreadable encrypted data");
}

TEST(EngineAutoWordTest, TrendingIngestSaveFailureRetainsWordsForRetry) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("learning.tsv"));
  const auto blocked_parent = temp.File("blocked");
  {
    std::ofstream file(blocked_parent);
    ASSERT_TRUE(file.is_open());
    file << "block directory creation";
  }
  const azookey::learning::test::TestByteCrypto crypto;
  azookey::learning::AutoWordStore auto_words(
      std::filesystem::path(blocked_parent) / "auto_words.tsv", &crypto);
  auto engine = MakeEngine(store);
  engine->SetAutoWordStore(&auto_words);
  const std::vector<azookey::learning::AutoWord> words{{"新語", "しんご"}};
  EXPECT_FALSE(engine->IngestTrendingWords(words, kNowBase, true));
  EXPECT_EQ(auto_words.LookupConfirmed("しんご").size(), 1u);
  ASSERT_TRUE(std::filesystem::remove(blocked_parent));
  ASSERT_TRUE(std::filesystem::create_directory(blocked_parent));
  ASSERT_TRUE(engine->IngestTrendingWords(words, kNowBase + 1, true));
  azookey::learning::AutoWordStore reloaded(auto_words.path(), &crypto);
  ASSERT_TRUE(reloaded.Load());
  EXPECT_EQ(reloaded.SerializeText(), auto_words.SerializeText());
}

TEST(EngineTypoCorrectionTest, ApplyConfigCarriesTheTypoAndMiningSettings) {
  ScopedTempDirectory temp;
  azookey::learning::LearningStore store(temp.File("azookey_engine_applyconfig_learning.tsv"),
                                         &azookey::learning::test::Crypto());
  azookey::learning::TypoCorrectionStore typo(temp.File("azookey_engine_applyconfig_typo.tsv"),
                                              &azookey::learning::test::Crypto());
  azookey::learning::AutoWordStore auto_words(temp.File("azookey_engine_applyconfig_words.tsv"),
                                              &azookey::learning::test::Crypto());

  azookey::host::EngineConfig cfg;
  cfg.typo_correction_mode = "suggest";
  cfg.auto_word_mining_enabled = true;
  auto engine = MakeEngine(store, cfg);
  engine->SetTypoStore(&typo);
  engine->SetAutoWordStore(&auto_words);

  // UpdateConfig goes through ApplyConfig, which copies field by field. A field
  // it forgets would leave the setting effective only until the next restart.
  auto updated = engine->config();
  updated.typo_correction_mode = "off";
  updated.typo_min_count = 9;
  updated.auto_word_mining_enabled = false;
  updated.auto_word_auto_register = true;
  updated.auto_word_min_count = 11;
  engine->ApplyConfig(updated);

  const auto applied = engine->config();
  EXPECT_EQ(applied.typo_correction_mode, "off");
  EXPECT_EQ(applied.typo_min_count, 9u);
  EXPECT_FALSE(applied.auto_word_mining_enabled);
  EXPECT_TRUE(applied.auto_word_auto_register);
  EXPECT_EQ(applied.auto_word_min_count, 11u);

  // And the new values actually take effect.
  EXPECT_FALSE(engine->ObserveTypo("こんちには", "こんにちは", kNowBase));
  engine->CommitObservation("あずきー", "アズーキー", kNowBase);
  EXPECT_EQ(auto_words.Size(), 0u);
}

// M47 section 8.5.1: the engine drives the model rows of the health machine
// from its own load outcomes.
TEST(InferenceEngineTest, ModelLoadOutcomesDriveTheHealthState) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }
  const std::string lpath = TempPath("azookey_host_engine_health_state.tsv");
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);
  EXPECT_EQ(engine->health_state(), azookey::host::HealthState::Healthy);

  azookey::host::ModelLoadOptions missing;
  missing.path = TempPath("azookey_host_engine_health_missing.gguf");
  std::remove(missing.path.c_str());
  missing.backend = azookey::host::BackendKind::Cpu;
  EXPECT_FALSE(engine->LoadModelWithResult(missing).ok);
  EXPECT_EQ(engine->health_state(), azookey::host::HealthState::DegradedModel);
  EXPECT_EQ(engine->health_snapshot().health_state, azookey::host::HealthState::DegradedModel);

  // Accepted, then failed again: back to DegradedModel through RecoveringModel.
  EXPECT_FALSE(engine->LoadModelWithResult(missing).ok);
  EXPECT_EQ(engine->health_state(), azookey::host::HealthState::DegradedModel);

  const std::string model_path = TempPath("azookey_host_engine_health_model.gguf");
  WriteMinimalGguf(model_path);
  azookey::host::ModelLoadOptions valid;
  valid.path = model_path;
  valid.backend = azookey::host::BackendKind::Cpu;
  EnableMockZenzaiCandidatesForTests(valid);
  std::optional<azookey::host::HealthState> during_load;
  valid.before_probe_for_tests = [&] { during_load = engine->health_state(); };
  ASSERT_TRUE(engine->LoadModelWithResult(valid).ok);
  EXPECT_EQ(during_load, azookey::host::HealthState::RecoveringModel);
  EXPECT_EQ(engine->health_state(), azookey::host::HealthState::Healthy);

  // With a model serving, a failed swap keeps it and is not a degradation.
  EXPECT_FALSE(engine->LoadModelWithResult(missing).ok);
  EXPECT_TRUE(engine->model_loaded());
  EXPECT_EQ(engine->health_state(), azookey::host::HealthState::Healthy);

  engine.reset();
  std::remove(model_path.c_str());
  RemoveProtectedStoreFile(lpath);
}

// A SafeMode that begins while the GGUF is being loaded, after the check at the
// start of the load, still keeps the loaded model from going live.
TEST(InferenceEngineTest, SafeModeEnteredDuringALoadDiscardsTheModel) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }
  const std::string lpath = TempPath("azookey_host_engine_health_safe_mode_race.tsv");
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);

  const std::string model_path = TempPath("azookey_host_engine_health_safe_mode_race.gguf");
  WriteMinimalGguf(model_path);
  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  options.backend = azookey::host::BackendKind::Cpu;
  EnableMockZenzaiCandidatesForTests(options);
  options.before_load_for_tests = [&](azookey::host::BackendKind) {
    engine->RestoreHealthState(azookey::host::HealthState::SafeMode);
  };

  const auto result = engine->LoadModelWithResult(options);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "safe_mode");
  EXPECT_FALSE(engine->model_loaded());
  EXPECT_EQ(engine->health_state(), azookey::host::HealthState::SafeMode);

  engine.reset();
  std::remove(model_path.c_str());
  RemoveProtectedStoreFile(lpath);
}

TEST(InferenceEngineTest, SafeModeIsNotLeftByModelEvents) {
  const std::string lpath = TempPath("azookey_host_engine_health_safe_mode.tsv");
  RemoveProtectedStoreFile(lpath);
  azookey::learning::LearningStore store(lpath, &azookey::learning::test::Crypto());
  auto engine = MakeEngine(store);
  ASSERT_TRUE(engine->ApplyHealthEvent(azookey::host::HealthEvent::CrashLoopDetected));
  EXPECT_EQ(engine->health_state(), azookey::host::HealthState::SafeMode);

  azookey::host::ModelLoadOptions missing;
  missing.path = TempPath("azookey_host_engine_health_safe_missing.gguf");
  std::remove(missing.path.c_str());
  // Refused before any probe: SafeMode runs no model, whichever caller asks.
  const auto refused = engine->LoadModelWithResult(missing);
  EXPECT_FALSE(refused.ok);
  EXPECT_EQ(refused.error, "safe_mode");
  EXPECT_EQ(engine->health_state(), azookey::host::HealthState::SafeMode);
  EXPECT_FALSE(engine->ApplyHealthEvent(azookey::host::HealthEvent::ModelLoadConfirmed));

  engine->RestoreHealthState(azookey::host::HealthState::SafeMode);
  EXPECT_TRUE(engine->ApplyHealthEvent(azookey::host::HealthEvent::SafeModeCleared));
  EXPECT_EQ(engine->health_state(), azookey::host::HealthState::Healthy);

  engine.reset();
  RemoveProtectedStoreFile(lpath);
}

// M54: the scorer stays behind an internal switch. Three commits lift a 2.0
// candidate by 2.4 under the M7 reranker (to 4.4, first) but only by log(4)
// under UserLearningScorer (to 3.39, still below 4.0).
TEST(InferenceEngineTest, UserLearningScorerRanksOnlyWhenSwitchedOn) {
  const auto rank = [](bool scorer_enabled, const char* file) {
    const std::string path = TempPath(file);
    RemoveProtectedStoreFile(path);
    azookey::learning::LearningStore store(path, &azookey::learning::test::Crypto());
    azookey::host::EngineConfig config;
    config.user_learning_scorer_enabled = scorer_enabled;
    config.learning_min_weight = 0.0;
    azookey::host::InferenceEngine engine(std::make_unique<ContextCapturingConverter>(), &store,
                                          config);
    for (int i = 1; i <= 3; ++i) engine.CommitObservation("かな", "候補3", kNowBase + i);
    auto candidates = engine.QueryCandidates("かな", "", kNowBase + 3);
    engine.FlushLearningStore();
    RemoveProtectedStoreFile(path);
    return candidates;
  };

  EXPECT_FALSE(azookey::host::EngineConfig{}.user_learning_scorer_enabled);
  const auto legacy = rank(false, "azookey_host_engine_scorer_off.tsv");
  ASSERT_FALSE(legacy.empty());
  EXPECT_EQ(legacy.front().surface, "候補3");
  const auto scored = rank(true, "azookey_host_engine_scorer_on.tsv");
  ASSERT_FALSE(scored.empty());
  EXPECT_EQ(scored.front().surface, "候補1");
}
