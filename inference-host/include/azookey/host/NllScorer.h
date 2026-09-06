#pragma once

#include <functional>
#include <span>
#include <string_view>

#include "azookey/core/IConverter.h"

namespace azookey::host {

struct NllConfig {
  bool enabled{false};
  int top_k{8};
  double weight{0.15};
  int budget_ms{20};
  int failure_threshold{3};
  bool operator==(const NllConfig&) const = default;
};

NllConfig ClampNllConfig(NllConfig config);
double NllPerCharacter(std::span<const double> logprobs, std::string_view surface);
std::vector<size_t> SelectNllCandidates(const std::vector<core::Candidate>& candidates, int top_k);
// Updates in place; RerankWithNll owns the copy used for all-or-nothing application.
size_t ApplyNllScores(std::vector<core::Candidate>& candidates, std::span<const size_t> indices,
                      std::span<const double> scores, double weight);

// Each row predicts the NEXT token. Prefix output is copied before any suffix decode.
class NllDecoder {
 public:
  virtual ~NllDecoder() = default;
  virtual std::vector<int32_t> TokenizeSurface(std::string_view surface) = 0;
  virtual std::vector<float> DecodePrefix() = 0;
  virtual std::vector<std::vector<float>> DecodeSuffix(std::span<const int32_t> tokens) = 0;
  virtual void Rollback() = 0;
};

struct NllEvaluation {
  std::vector<double> scores;
  double prefix_ms{};
};
NllEvaluation EvaluateNll(NllDecoder& decoder, std::span<const std::string> surfaces,
                          const core::ConversionContext& context);

struct NllOutcome {
  std::string reason;
  size_t targets{};
  size_t applied{};
  double prefix_ms{};
  double elapsed_ms{};
};

// Owned by a converter, serialized by converter_call_mutex_. Reload creates fresh state.
struct NllCircuit {
  uint64_t revision{};
  int failures{};
};
using NllEvaluator =
    std::function<NllEvaluation(std::span<const std::string>, const core::ConversionContext&)>;
using NllClock = std::function<std::chrono::steady_clock::time_point()>;
NllOutcome RerankWithNll(std::vector<core::Candidate>& candidates, NllConfig config,
                         NllCircuit& circuit, uint64_t revision,
                         const core::ConversionContext& context, const NllEvaluator& evaluate,
                         const NllClock& now = std::chrono::steady_clock::now);

}  // namespace azookey::host
