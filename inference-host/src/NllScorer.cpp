#include "azookey/host/NllScorer.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

#include "azookey/core/Utf8.h"

namespace azookey::host {
namespace {
bool Canceled(const core::ConversionContext& context) {
  return context.cancel && context.cancel->load(std::memory_order_relaxed);
}
bool Expired(const core::ConversionContext& context) {
  return context.deadline && std::chrono::steady_clock::now() >= *context.deadline;
}
void CheckAbort(const core::ConversionContext& context) {
  if (Canceled(context) || Expired(context)) throw std::runtime_error("nll aborted");
}
struct LogNormalization {
  double maximum;
  double log_sum;
};
LogNormalization LogNormalizer(std::span<const float> logits) {
  if (logits.empty()) throw std::runtime_error("missing nll logits");
  double maximum = -std::numeric_limits<double>::infinity();
  for (const float value : logits) {
    if (!std::isfinite(value)) return {0, std::numeric_limits<double>::quiet_NaN()};
    maximum = std::max(maximum, static_cast<double>(value));
  }
  double sum = 0;
  for (const float value : logits) sum += std::exp(static_cast<double>(value) - maximum);
  return {maximum, std::log(sum)};
}
double LogProbability(std::span<const float> logits, LogNormalization normalizer, int32_t token) {
  if (token < 0 || static_cast<size_t>(token) >= logits.size())
    throw std::runtime_error("invalid nll token");
  return (static_cast<double>(logits[static_cast<size_t>(token)]) - normalizer.maximum) -
         normalizer.log_sum;
}
}  // namespace

NllConfig ClampNllConfig(NllConfig config) {
  config.top_k = std::clamp(config.top_k, 1, 16);
  config.weight = std::isfinite(config.weight) ? std::clamp(config.weight, 0.0, 1.0) : 0.15;
  config.budget_ms = std::clamp(config.budget_ms, 5, 60);
  config.failure_threshold = std::clamp(config.failure_threshold, 1, 10);
  return config;
}

double NllPerCharacter(std::span<const double> logprobs, std::string_view surface) {
  double total = 0;
  for (const double value : logprobs) {
    if (!std::isfinite(value)) return std::numeric_limits<double>::quiet_NaN();
    total -= value;
  }
  size_t count = 0, offset = 0;
  char32_t codepoint{};
  while (offset < surface.size()) {
    if (!core::DecodeNextUtf8(surface, offset, codepoint))
      return std::numeric_limits<double>::quiet_NaN();
    ++count;
  }
  return total / static_cast<double>(std::max<size_t>(1, count));
}

std::vector<size_t> SelectNllCandidates(const std::vector<core::Candidate>& candidates, int top_k) {
  std::vector<size_t> indices;
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (candidates[i].source == core::CandidateSource::SystemDictionary ||
        candidates[i].source == core::CandidateSource::Heuristic)
      indices.push_back(i);
  }
  std::stable_sort(indices.begin(), indices.end(),
                   [&](size_t a, size_t b) { return candidates[a].score > candidates[b].score; });
  indices.resize(std::min(indices.size(), static_cast<size_t>(std::clamp(top_k, 1, 16))));
  return indices;
}

size_t ApplyNllScores(std::vector<core::Candidate>& candidates, std::span<const size_t> indices,
                      std::span<const double> scores, double weight) {
  if (indices.size() != scores.size()) throw std::runtime_error("nll score count mismatch");
  double minimum = std::numeric_limits<double>::infinity();
  for (const double value : scores)
    if (std::isfinite(value)) minimum = std::min(minimum, value);
  if (!std::isfinite(minimum)) return 0;
  // Prepare all string allocations before committing any changes.
  auto updated = candidates;
  size_t applied = 0;
  for (size_t i = 0; i < indices.size(); ++i) {
    if (!std::isfinite(scores[i])) continue;
    const double delta = std::max(minimum - scores[i], -2.0) * weight;
    auto& candidate = updated.at(indices[i]);
    std::ostringstream tag;
    tag.imbue(std::locale::classic());
    tag << std::fixed << std::setprecision(6) << "nll=" << scores[i] << ";nlld=" << delta;
    if (!candidate.debug_info.empty()) candidate.debug_info += ';';
    candidate.debug_info += tag.str();
    candidate.score += delta;
    ++applied;
  }
  candidates.swap(updated);
  return applied;
}

NllEvaluation EvaluateNll(NllDecoder& decoder, std::span<const std::string> surfaces,
                          const core::ConversionContext& context) {
  CheckAbort(context);
  NllEvaluation result;
  const auto start = std::chrono::steady_clock::now();
  const auto prefix_logits = decoder.DecodePrefix();
  result.prefix_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  const auto prefix_normalizer = LogNormalizer(prefix_logits);
  for (const auto& surface : surfaces) {
    CheckAbort(context);
    const auto tokens = decoder.TokenizeSurface(surface);
    if (tokens.empty()) throw std::runtime_error("empty nll tokenization");
    std::vector<double> logprobs;
    logprobs.push_back(LogProbability(prefix_logits, prefix_normalizer, tokens.front()));
    const auto rows = decoder.DecodeSuffix(std::span(tokens).first(tokens.size() - 1));
    if (rows.size() != tokens.size() - 1) throw std::runtime_error("missing nll rows");
    for (size_t i = 0; i < rows.size(); ++i)
      logprobs.push_back(LogProbability(rows[i], LogNormalizer(rows[i]), tokens[i + 1]));
    result.scores.push_back(NllPerCharacter(logprobs, surface));
    decoder.Rollback();
    CheckAbort(context);
  }
  return result;
}

NllOutcome RerankWithNll(std::vector<core::Candidate>& candidates, NllConfig config,
                         NllCircuit& circuit, uint64_t revision,
                         const core::ConversionContext& context, const NllEvaluator& evaluate,
                         const NllClock& now) {
  NllOutcome result;
  if (!config.enabled || context.live) {
    result.reason = "disabled";
    return result;
  }
  if (Canceled(context)) return result;
  config = ClampNllConfig(config);
  if (circuit.revision != revision) circuit = {revision, 0};
  const auto start = now();
  try {
    const auto indices = SelectNllCandidates(candidates, config.top_k);
    result.targets = indices.size();
    if (indices.empty()) {
      result.reason = "no_target";
      return result;
    }
    if (circuit.failures >= config.failure_threshold) {
      result.reason = "circuit_open";
      return result;
    }
    auto bounded = context;
    const auto deadline = start + std::chrono::milliseconds(config.budget_ms);
    bounded.deadline = context.deadline ? std::min(*context.deadline, deadline) : deadline;
    auto expired = [&] { return now() >= *bounded.deadline; };
    std::vector<std::string> surfaces;
    for (const auto index : indices) surfaces.push_back(candidates[index].surface);
    NllEvaluation evaluation;
    try {
      if (Canceled(bounded) || expired()) throw std::runtime_error("nll aborted");
      evaluation = evaluate(surfaces, bounded);
    } catch (...) {
      if (Canceled(bounded)) return result;
      result.reason = expired() ? "budget_exceeded" : "infer_error";
    }
    if (Canceled(bounded)) return result;
    if (expired()) result.reason = "budget_exceeded";
    if (result.reason.empty()) {
      result.prefix_ms = evaluation.prefix_ms;
      // Include application preparation in the all-or-nothing budget.
      auto updated = candidates;
      const auto applied = ApplyNllScores(updated, indices, evaluation.scores, config.weight);
      if (Canceled(bounded)) return result;
      if (expired())
        result.reason = "budget_exceeded";
      else {
        candidates.swap(updated);
        result.applied = applied;
        result.reason = applied ? "applied" : "invalid_score";
        circuit.failures = 0;
      }
    }
  } catch (...) {
    if (Canceled(context)) return result;
    result.reason = "infer_error";
  }
  if (result.reason == "budget_exceeded" || result.reason == "infer_error") ++circuit.failures;
  result.elapsed_ms = std::chrono::duration<double, std::milli>(now() - start).count();
  return result;
}
}  // namespace azookey::host
