#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "MockTinyRerankerRuntime.h"
#include "azookey/core/PlatformPaths.h"

namespace azookey::reranker {
namespace {

using namespace std::chrono_literals;
using Outcome = TinyRerankOutcome;
using test::MockTinyRerankerRuntime;

class TinyRerankerRuntimeTest : public testing::Test {
 protected:
  void SetUp() override {
    auto base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 16; ++attempt) {
      const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
      auto path = base / ("azookey_tiny_reranker_" + std::to_string(suffix) + "_" +
                          std::to_string(attempt));
      if (std::filesystem::create_directory(path)) {
        temp_ = std::move(path);
        break;
      }
    }
    ASSERT_FALSE(temp_.empty());
    model_ = temp_ / "model.onnx";
    std::ofstream(model_).put('x');
    config_.enabled = true;
    config_.model_path = core::PathToUtf8(model_);
    runtime_.SetConfig(config_);
  }

  void TearDown() override {
    std::error_code error;
    if (!temp_.empty()) std::filesystem::remove_all(temp_, error);
  }

  void Install(MockTinyRerankerRuntime::Forward forward, std::chrono::milliseconds timeout = 2s) {
    auto mock = std::make_shared<MockTinyRerankerRuntime>(std::move(forward));
    runtime_.SetFactoryForTests([mock](const auto&) { return mock; }, timeout);
  }

  std::filesystem::path temp_;
  std::filesystem::path model_;
  TinyRerankerConfig config_;
  TinyRerankerRuntime runtime_;
};

TEST_F(TinyRerankerRuntimeTest, ResolvesModelPathsInOrderAndDoesNotFallBackFromAnOverride) {
  const auto user = temp_ / "user";
  const auto bundle = temp_ / "bundle";
  ASSERT_TRUE(std::filesystem::create_directory(user));
  ASSERT_TRUE(std::filesystem::create_directory(bundle));
  const auto user_model = user / "tiny_reranker.onnx";
  const auto bundled_model = bundle / "tiny_reranker.onnx";
  std::ofstream(bundled_model).put('x');
  EXPECT_EQ(ResolveTinyRerankerModelPath("", user, bundle), bundled_model);
  std::ofstream(user_model).put('x');
  EXPECT_EQ(ResolveTinyRerankerModelPath("", user, bundle), user_model);
  EXPECT_EQ(ResolveTinyRerankerModelPath(core::PathToUtf8(model_), user, bundle), model_);
  EXPECT_EQ(ResolveTinyRerankerModelPath(core::PathToUtf8(temp_ / "missing"), user, bundle),
            std::nullopt);
}

TEST_F(TinyRerankerRuntimeTest, RelativeOverrideFailsEvenWhenItNamesAnExistingFile) {
  const auto relative = temp_.filename() / "model.onnx";
  ASSERT_FALSE(relative.is_absolute());
  ASSERT_TRUE(std::filesystem::create_directory(relative.parent_path()));
  struct RelativeModelCleanup {
    std::filesystem::path model;
    ~RelativeModelCleanup() {
      std::error_code error;
      std::filesystem::remove(model, error);
      std::filesystem::remove(model.parent_path(), error);
    }
  } cleanup{relative};
  std::ofstream(relative).put('x');
  ASSERT_TRUE(std::filesystem::is_regular_file(relative));
  Install([](auto rows, const auto&) { return std::vector<float>(rows.size(), 1.0f); });
  config_.model_path = core::PathToUtf8(relative);
  runtime_.SetConfig(config_);

  std::vector<core::Candidate> candidates{{"先", "", 0.5}};
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::LoadFailed);
  EXPECT_DOUBLE_EQ(candidates.front().score, 0.5);
}

TEST_F(TinyRerankerRuntimeTest, DisabledSecureAndAbsentRuntimePreserveTheCandidates) {
  std::vector<core::Candidate> candidates{{"先", "さき", 0.5}, {"後", "あと", 1.0}};
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::LoadFailed);
  int loads = 0;
  runtime_.SetFactoryForTests([&loads](const auto&) {
    ++loads;
    return std::shared_ptr<ITinyRerankerRuntime>{};
  });
  EXPECT_EQ(runtime_.Apply(candidates, true), Outcome::SecureMode);
  EXPECT_EQ(loads, 0);
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::LoadFailed);
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::LoadFailed);
  EXPECT_EQ(loads, 1);
  config_.enabled = false;
  runtime_.SetConfig(config_);
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::Disabled);
  EXPECT_EQ(loads, 1);
  EXPECT_EQ(candidates[0].surface, "先");
  EXPECT_DOUBLE_EQ(candidates[0].score, 0.5);
  EXPECT_EQ(candidates[1].surface, "後");
  EXPECT_DOUBLE_EQ(candidates[1].score, 1.0);
}

TEST_F(TinyRerankerRuntimeTest, ScoresOnlyTheTopThirtyWithStableTiesAndClampsTheAddition) {
  std::vector<core::Candidate> candidates;
  for (int index = 0; index < 30; ++index) {
    candidates.push_back({std::to_string(index), "", 1.0});
  }
  candidates.push_back({"新候補", "", 2.0});
  auto observed = std::make_shared<std::vector<std::array<float, 12>>>();
  Install([observed](auto rows, const auto&) {
    observed->assign(rows.begin(), rows.end());
    auto scores = std::vector<float>(rows.size(), 2.0f);
    scores.front() = -1.0f;
    return scores;
  });
  ASSERT_EQ(runtime_.Apply(candidates, false), Outcome::Applied);
  ASSERT_EQ(observed->size(), 30u);
  EXPECT_EQ(observed->front()[6], 3.0f);
  EXPECT_EQ(candidates.front().surface, "新候補");
  EXPECT_DOUBLE_EQ(candidates.front().score, 2.0);
  for (int index = 0; index < 29; ++index) {
    const auto& candidate = candidates[static_cast<size_t>(index) + 1];
    EXPECT_EQ(candidate.surface, std::to_string(index));
    EXPECT_DOUBLE_EQ(candidate.score, 1.15);
  }
  EXPECT_EQ(candidates.back().surface, "29");
  EXPECT_DOUBLE_EQ(candidates.back().score, 1.0);
}

TEST_F(TinyRerankerRuntimeTest, InvalidFeaturesSkipOnlyTheirRowsAndDoNotTripTheBreaker) {
  config_.failure_threshold = 1;
  runtime_.SetConfig(config_);
  std::vector<core::Candidate> candidates{{"欠損", "", 0.7}, {"有効", "", 0.6}};
  auto observed_count = std::make_shared<std::atomic<size_t>>(0);
  Install([observed_count](auto rows, const auto&) {
    *observed_count = rows.size();
    return std::vector<float>(rows.size(), 1.0f);
  });
  candidates[0].rerank_signals.dictionary_score = std::numeric_limits<double>::infinity();
  candidates[1].rerank_signals.dictionary_score = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::InvalidInput);
  EXPECT_EQ(observed_count->load(), 0u);
  candidates[1].rerank_signals.dictionary_score.reset();
  ASSERT_EQ(runtime_.Apply(candidates, false), Outcome::Applied);
  EXPECT_EQ(observed_count->load(), 1u);
  EXPECT_EQ(candidates[0].surface, "有効");
  EXPECT_DOUBLE_EQ(candidates[0].score, 0.75);
  EXPECT_DOUBLE_EQ(candidates[1].score, 0.7);
}

TEST_F(TinyRerankerRuntimeTest, MalformedOutputsAreAtomicAndSuccessAndReloadResetTheBreaker) {
  std::vector<core::Candidate> candidates{{"先", "", 0.5}, {"後", "", 1.0}};
  auto calls = std::make_shared<std::atomic<int>>(0);
  Install([calls](auto rows, const auto&) -> std::vector<float> {
    switch ((*calls)++) {
      case 1:
        return std::vector<float>(rows.size(), 0.0f);
      case 2:
        return {1.0f};
      case 3:
        return {1.0f, std::numeric_limits<float>::quiet_NaN()};
      default:
        throw std::runtime_error("mock failure");
    }
  });
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::InferError);
  EXPECT_EQ(candidates[0].surface, "先");
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::Applied);
  for (int failure = 0; failure < 3; ++failure) {
    EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::InferError);
    EXPECT_EQ(candidates[0].surface, "後");
    EXPECT_DOUBLE_EQ(candidates[0].score, 1.0);
    EXPECT_DOUBLE_EQ(candidates[1].score, 0.5);
  }
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::CircuitOpen);
  EXPECT_EQ(calls->load(), 5);
  Install([](auto rows, const auto&) { return std::vector<float>(rows.size(), 1.0f); });
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::Applied);
  EXPECT_DOUBLE_EQ(candidates[0].score, 1.15);
}

TEST_F(TinyRerankerRuntimeTest, WatchdogDiscardsTheLateResultAndOpensTheBreaker) {
  config_.failure_threshold = 1;
  runtime_.SetConfig(config_);
  auto stopped = std::make_shared<std::promise<void>>();
  auto completion = stopped->get_future();
  Install(
      [stopped](auto rows, const auto& stop) {
        stop.wait(false, std::memory_order_relaxed);
        stopped->set_value();
        return std::vector<float>(rows.size(), 1.0f);
      },
      10ms);

  std::vector<core::Candidate> candidates{{"先", "", 0.5}, {"後", "", 1.0}};
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::Timeout);
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::CircuitOpen);
  EXPECT_EQ(completion.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(candidates[0].surface, "先");
  EXPECT_DOUBLE_EQ(candidates[0].score, 0.5);
  EXPECT_DOUBLE_EQ(candidates[1].score, 1.0);
}

TEST_F(TinyRerankerRuntimeTest, ChangedConfigClosesAnOpenBreaker) {
  config_.failure_threshold = 1;
  runtime_.SetConfig(config_);
  auto calls = std::make_shared<std::atomic<int>>(0);
  Install([calls](auto rows, const auto&) {
    if ((*calls)++ == 0) throw std::runtime_error("mock failure");
    return std::vector<float>(rows.size(), 1.0f);
  });

  std::vector<core::Candidate> candidates{{"先", "", 0.5}};
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::InferError);
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::CircuitOpen);
  runtime_.SetConfig(config_);
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::CircuitOpen);
  config_.timeout_ms = kTinyTimeoutMaxMs;
  runtime_.SetConfig(config_);
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::Applied);
  EXPECT_EQ(calls->load(), 2);
}

TEST_F(TinyRerankerRuntimeTest, ShortRequestBudgetSkipsWithoutStartingForward) {
  config_.failure_threshold = 1;
  runtime_.SetConfig(config_);
  const auto now = std::chrono::steady_clock::now();
  runtime_.SetClockForTests([now] { return now; });
  auto calls = std::make_shared<std::atomic<int>>(0);
  Install([calls](auto rows, const auto&) {
    ++*calls;
    return std::vector<float>(rows.size(), 1.0f);
  });

  std::vector<core::Candidate> candidates{{"先", "", 0.5}};
  EXPECT_EQ(runtime_.Apply(candidates, false, nullptr, now + 20ms), Outcome::Skipped);
  EXPECT_EQ(calls->load(), 0);
  EXPECT_DOUBLE_EQ(candidates.front().score, 0.5);
  runtime_.SetClockForTests({});
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::Applied);
}

TEST_F(TinyRerankerRuntimeTest, RequestDeadlineStopsForwardAndPendingWorkDoesNotCount) {
  config_.failure_threshold = 1;
  runtime_.SetConfig(config_);
  auto started = std::make_shared<std::promise<void>>();
  auto entered = started->get_future();
  auto stopped = std::make_shared<std::promise<void>>();
  auto saw_stop = stopped->get_future();
  std::promise<void> release;
  auto gate = release.get_future().share();
  auto mock = std::make_shared<MockTinyRerankerRuntime>(
      [started, stopped, gate](auto rows, const auto& stop) {
        started->set_value();
        stop.wait(false, std::memory_order_relaxed);
        stopped->set_value();
        gate.wait();
        return std::vector<float>(rows.size(), 1.0f);
      });
  // Only the injected clock is before this deadline, so the real future wait
  // cannot block while waiting for the worker to advance the simulated time.
  const auto deadline = std::chrono::steady_clock::now();
  auto now = deadline - 3s;
  runtime_.SetFactoryForTests(
      [&now, deadline, mock](const auto&) {
        // Model loading uses most of the request budget.
        now = deadline - 1ms;
        return mock;
      },
      2s);
  runtime_.SetClockForTests([&now, deadline, &entered] {
    return entered.wait_for(0ms) == std::future_status::ready ? deadline : now;
  });

  std::vector<core::Candidate> candidates{{"先", "", 0.5}};
  EXPECT_EQ(runtime_.Apply(candidates, false, nullptr, deadline), Outcome::Skipped);
  EXPECT_EQ(saw_stop.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::Skipped);
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::Skipped);
  EXPECT_DOUBLE_EQ(candidates.front().score, 0.5);
  release.set_value();
}

TEST_F(TinyRerankerRuntimeTest, PendingTinyTimeoutCountsOncePerRequest) {
  config_.failure_threshold = 2;
  runtime_.SetConfig(config_);
  std::promise<void> release;
  auto gate = release.get_future().share();
  Install(
      [gate](auto rows, const auto&) {
        gate.wait();
        return std::vector<float>(rows.size(), 1.0f);
      },
      10ms);

  std::vector<core::Candidate> dictionary{{"辞書", "", 0.5}};
  std::vector<core::Candidate> predictions{{"予測", "", 0.7}};
  EXPECT_EQ(runtime_.ApplyPredictionBatches(dictionary, predictions, false, nullptr),
            Outcome::Timeout);
  EXPECT_EQ(runtime_.Apply(predictions, false), Outcome::Timeout);
  EXPECT_EQ(runtime_.Apply(predictions, false), Outcome::CircuitOpen);
  EXPECT_DOUBLE_EQ(dictionary.front().score, 0.5);
  EXPECT_DOUBLE_EQ(predictions.front().score, 0.7);
  release.set_value();
}

TEST_F(TinyRerankerRuntimeTest, PendingCanceledForwardCountsOnceItReachesTinyTimeout) {
  config_.failure_threshold = 2;
  runtime_.SetConfig(config_);
  constexpr auto timeout = 2s;
  const auto start = std::chrono::steady_clock::now();
  auto now = start;
  runtime_.SetClockForTests([&now] { return now; });
  auto canceled = std::make_shared<std::atomic<bool>>(false);
  auto stopped = std::make_shared<std::promise<void>>();
  auto saw_stop = stopped->get_future();
  std::promise<void> release;
  auto gate = release.get_future().share();
  Install(
      [canceled, stopped, gate](auto rows, const auto& stop) {
        canceled->store(true, std::memory_order_relaxed);
        stop.wait(false, std::memory_order_relaxed);
        stopped->set_value();
        gate.wait();
        return std::vector<float>(rows.size(), 1.0f);
      },
      timeout);

  std::vector<core::Candidate> candidates{{"先", "", 0.5}};
  EXPECT_EQ(runtime_.Apply(candidates, false, canceled.get()), Outcome::Skipped);
  EXPECT_EQ(saw_stop.wait_for(2s), std::future_status::ready);
  now = start + timeout - 1ms;
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::Skipped);
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::Skipped);
  now = start + timeout;
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::Timeout);
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::Timeout);
  EXPECT_EQ(runtime_.Apply(candidates, false), Outcome::CircuitOpen);
  EXPECT_DOUBLE_EQ(candidates.front().score, 0.5);
  release.set_value();
}

}  // namespace
}  // namespace azookey::reranker
