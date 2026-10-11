#pragma once

#include <cstdint>
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

struct NllSuffixBatch {
  size_t candidates{};
  size_t tokens{};
};
// Select a leading group whose suffixes fit the free unified KV cells and sequence IDs.
NllSuffixBatch PlanNllSuffixBatch(std::span<const std::vector<int32_t>> tokens, size_t free_cells,
                                  size_t max_sequences);

// Each row predicts the NEXT token. Prefix output is copied before any suffix decode.
class NllDecoder {
 public:
  // The row predicts tokens[candidate][position + 1]. Consume it synchronously:
  // the span is borrowed and expires when the callback returns.
  using SuffixRow =
      std::function<void(size_t candidate, size_t position, std::span<const float> logits)>;
  virtual ~NllDecoder() = default;
  virtual std::vector<int32_t> TokenizeSurface(std::string_view surface) = 0;
  virtual std::vector<float> DecodePrefix() = 0;
  // Full token sequences, including the final token (which must not be decoded).
  // Emit each candidate's rows in position order; candidates may be interleaved.
  // On success release all suffix KV, preserving the prompt and its snapshot.
  virtual void DecodeSuffixes(std::span<const std::vector<int32_t>> tokens,
                              const SuffixRow& consume) = 0;
};

struct NllEvaluation {
  std::vector<double> scores;
  double prefix_ms{};
  // Prompt tokens served from the KV cache that generation, or a previous evaluation, left
  // behind instead of being decoded again.
  uint64_t prefix_reused_tokens{};
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
