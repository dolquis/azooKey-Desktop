#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/NllScorer.h"

namespace {
using namespace azookey::host;
using azookey::core::Candidate;
using azookey::core::CandidateSource;
using azookey::core::ConversionContext;

Candidate MakeCandidate(std::string surface, double score, CandidateSource source) {
  Candidate result;
  result.surface = std::move(surface);
  result.reading = "reading";
  result.score = score;
  result.source = source;
  result.debug_info = "original";
  return result;
}
std::vector<Candidate> Candidates() {
  return {MakeCandidate("AA", 1.0, CandidateSource::SystemDictionary),
          MakeCandidate("B", 0.95, CandidateSource::Heuristic),
          MakeCandidate("user", 1.5, CandidateSource::UserDictionary),
          MakeCandidate("model", 1.4, CandidateSource::Model),
          MakeCandidate("other", 0.94, CandidateSource::SystemDictionary),
          MakeCandidate("llm", 0.7, CandidateSource::Llm)};
}
void ExpectUnchanged(const std::vector<Candidate>& actual, const std::vector<Candidate>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < actual.size(); ++i) {
    EXPECT_EQ(actual[i].surface, expected[i].surface);
    EXPECT_EQ(actual[i].reading, expected[i].reading);
    EXPECT_EQ(actual[i].source, expected[i].source);
    EXPECT_DOUBLE_EQ(actual[i].score, expected[i].score);
    EXPECT_EQ(actual[i].debug_info, expected[i].debug_info);
  }
}

struct Decoder : NllDecoder {
  int prefix_calls{}, rollbacks{};
  std::vector<std::vector<int32_t>> decoded;
  std::vector<float> current{0.0f, 2.0f, -2.0f};
  std::vector<int32_t> TokenizeSurface(std::string_view surface) override {
    return surface == "AA" ? std::vector<int32_t>{1, 2, 0} : std::vector<int32_t>{2};
  }
  std::vector<float> DecodePrefix() override {
    ++prefix_calls;
    return current;
  }
  std::vector<std::vector<float>> DecodeSuffix(std::span<const int32_t> tokens) override {
    decoded.emplace_back(tokens.begin(), tokens.end());
    if (tokens.empty()) return {};
    current = {5.0f, -3.0f, 0.0f};
    return {{-1.0f, 0.0f, 3.0f}, current};
  }
  void Rollback() override { ++rollbacks; }  // KV rollback intentionally does not restore logits.
};

TEST(NllScorerTest, PrefixSnapshotAndNextTokenPositionsExcludeFinalDecode) {
  Decoder decoder;
  const std::array<std::string, 2> surfaces{"AA", "B"};
  const auto result = EvaluateNll(decoder, surfaces, {});
  ASSERT_EQ(result.scores.size(), 2u);
  const double prefix_z = std::log(1 + std::exp(2.0) + std::exp(-2.0));
  const double row1_z = std::log(std::exp(-1.0) + 1 + std::exp(3.0));
  const double row2_z = std::log(std::exp(5.0) + std::exp(-3.0) + 1);
  EXPECT_NEAR(result.scores[0], (prefix_z - 2 + row1_z - 3 + row2_z - 5) / 2, 1e-12);
  EXPECT_NEAR(result.scores[1], prefix_z + 2, 1e-12);
  EXPECT_EQ(decoder.prefix_calls, 1);
  EXPECT_EQ(decoder.rollbacks, 2);
  ASSERT_EQ(decoder.decoded.size(), 2u);
  EXPECT_EQ(decoder.decoded[0], (std::vector<int32_t>{1, 2}));
  EXPECT_TRUE(decoder.decoded[1].empty());
}

TEST(NllScorerTest, LogSoftmaxRemainsStableForLargeCommonLogitOffsets) {
  Decoder decoder;
  decoder.current = {1e30f, 1e30f, 1e30f};
  const auto result = EvaluateNll(decoder, std::array<std::string, 1>{"B"}, {});
  ASSERT_EQ(result.scores.size(), 1u);
  EXPECT_NEAR(result.scores[0], std::log(3.0), 1e-12);
}

TEST(NllScorerTest, NormalizesUnicodeScalarsRatherThanTokensOrBytes) {
  const std::array<double, 2> probabilities{-2.0, -4.0};
  EXPECT_DOUBLE_EQ(NllPerCharacter(probabilities, "日本語"), 2.0);
  EXPECT_DOUBLE_EQ(NllPerCharacter(probabilities, "😀"), 6.0);
  EXPECT_DOUBLE_EQ(NllPerCharacter(probabilities, "か\u3099"), 3.0);
  EXPECT_TRUE(std::isnan(NllPerCharacter(probabilities, "\xff")));
  EXPECT_TRUE(
      std::isnan(NllPerCharacter(std::array{0.0, std::numeric_limits<double>::infinity()}, "a")));
}

TEST(NllScorerTest, RelativePenaltyProtectsExcludedSourcesAndTopKOverflow) {
  auto candidates = Candidates();
  const auto before = candidates;
  const auto indices = SelectNllCandidates(candidates, 2);
  ASSERT_EQ(indices, (std::vector<size_t>{0, 1}));
  EXPECT_EQ(ApplyNllScores(candidates, indices, std::array{5.0, 1.0}, 0.15), 2u);
  EXPECT_DOUBLE_EQ(candidates[0].score, 0.7);
  EXPECT_DOUBLE_EQ(candidates[1].score, before[1].score);
  ExpectUnchanged({candidates.begin() + 2, candidates.end()}, {before.begin() + 2, before.end()});
  auto ranked_before = before;
  auto sort = [](auto& items) {
    std::stable_sort(items.begin(), items.end(),
                     [](const auto& a, const auto& b) { return a.score > b.score; });
  };
  sort(ranked_before);
  sort(candidates);
  for (size_t i = 2; i < before.size(); ++i) {
    auto position = [&](const auto& items) {
      return std::find_if(items.begin(), items.end(),
                          [&](const auto& c) { return c.surface == before[i].surface; }) -
             items.begin();
    };
    EXPECT_LE(position(candidates), position(ranked_before));
  }
}

TEST(NllScorerTest, NonfiniteCandidateIsUnchangedWhileOthersApply) {
  auto candidates = Candidates();
  const auto before = candidates;
  const auto indices = SelectNllCandidates(candidates, 3);
  EXPECT_EQ(ApplyNllScores(candidates, indices,
                           std::array{std::numeric_limits<double>::quiet_NaN(), 1.0, 2.0}, 0.15),
            2u);
  ExpectUnchanged({candidates[0]}, {before[0]});
  EXPECT_LT(candidates[4].score, before[4].score);
}

TEST(NllScorerTest, DisabledAndLiveNeverEvaluateOrChangeCandidates) {
  for (const bool live : {false, true}) {
    auto candidates = Candidates();
    const auto before = candidates;
    NllConfig config;
    config.enabled = live;
    ConversionContext context;
    context.live = live;
    NllCircuit circuit;
    int calls = 0;
    const auto result =
        RerankWithNll(candidates, config, circuit, 0, context, [&](auto, const auto&) {
          ++calls;
          return NllEvaluation{};
        });
    EXPECT_EQ(result.reason, "disabled");
    EXPECT_EQ(calls, 0);
    ExpectUnchanged(candidates, before);
  }
}

TEST(NllScorerTest, TimeoutAfterEvaluationDiscardsAllScoresAndOpensCircuit) {
  auto candidates = Candidates();
  const auto before = candidates;
  NllConfig config{true, 2};
  NllCircuit circuit;
  auto time = std::chrono::steady_clock::time_point{};
  int calls = 0;
  auto evaluate = [&](auto, const auto&) {
    ++calls;
    time += std::chrono::milliseconds(21);
    return NllEvaluation{{5.0, 1.0}, 1.0};
  };
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(
        RerankWithNll(candidates, config, circuit, 0, {}, evaluate, [&] { return time; }).reason,
        "budget_exceeded");
    ExpectUnchanged(candidates, before);
  }
  EXPECT_EQ(
      RerankWithNll(candidates, config, circuit, 0, {}, evaluate, [&] { return time; }).reason,
      "circuit_open");
  EXPECT_EQ(calls, 3);
  // Settings revision and model reload (fresh circuit) both permit evaluation again.
  EXPECT_EQ(
      RerankWithNll(candidates, config, circuit, 1, {}, evaluate, [&] { return time; }).reason,
      "budget_exceeded");
  NllCircuit reloaded;
  EXPECT_EQ(
      RerankWithNll(candidates, config, reloaded, 1, {}, evaluate, [&] { return time; }).reason,
      "budget_exceeded");
  EXPECT_EQ(calls, 5);
}

TEST(NllScorerTest, CancellationAndInvalidScoresDoNotCountAsRuntimeFailures) {
  auto candidates = Candidates();
  const auto before = candidates;
  NllConfig config{true, 2};
  NllCircuit circuit;
  std::atomic<bool> cancel{false};
  ConversionContext context;
  context.cancel = &cancel;
  auto result = RerankWithNll(candidates, config, circuit, 0, context, [&](auto, const auto&) {
    cancel = true;
    return NllEvaluation{{5, 1}};
  });
  EXPECT_TRUE(result.reason.empty());
  EXPECT_EQ(circuit.failures, 0);
  ExpectUnchanged(candidates, before);
  result = RerankWithNll(candidates, config, circuit, 0, {},
                         [](auto, const auto&) { return NllEvaluation{{NAN, INFINITY}}; });
  EXPECT_EQ(result.reason, "invalid_score");
  EXPECT_EQ(circuit.failures, 0);
  ExpectUnchanged(candidates, before);
}

TEST(NllScorerTest, RuntimeErrorsAreContainedAndSuccessResetsConsecutiveFailures) {
  auto candidates = Candidates();
  const auto before = candidates;
  NllConfig config{true, 2};
  NllCircuit circuit;
  auto fail = [](auto, const auto&) -> NllEvaluation { throw std::runtime_error("test"); };
  EXPECT_EQ(RerankWithNll(candidates, config, circuit, 0, {}, fail).reason, "infer_error");
  EXPECT_EQ(circuit.failures, 1);
  ExpectUnchanged(candidates, before);
  EXPECT_EQ(RerankWithNll(candidates, config, circuit, 0, {},
                          [](auto, const auto&) { return NllEvaluation{{5, 1}}; })
                .reason,
            "applied");
  EXPECT_EQ(circuit.failures, 0);
  std::vector<Candidate> empty;
  EXPECT_EQ(RerankWithNll(empty, config, circuit, 0, {}, fail).reason, "no_target");
}

TEST(NllScorerTest, EngineWithoutModelPreservesDefaultResultsAndHealth) {
  EngineConfig config;
  InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), nullptr, config);
  const auto before = engine.QueryCandidates("にほんご", "", 0);
  config.nll.enabled = true;
  engine.ApplyConfig(config);
  const auto after = engine.QueryCandidates("にほんご", "", 0);
  ExpectUnchanged(after, before);
  EXPECT_FALSE(engine.effective_last_error().has_value());
}
}  // namespace
