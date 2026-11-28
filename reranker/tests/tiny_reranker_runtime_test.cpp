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

}  // namespace
}  // namespace azookey::reranker
