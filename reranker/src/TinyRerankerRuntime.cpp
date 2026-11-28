#include "azookey/reranker/TinyRerankerRuntime.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <thread>
#include <utility>

#include "azookey/core/PlatformPaths.h"
#include "azookey/reranker/FeatureExtractor.h"

namespace azookey::reranker {
namespace {

constexpr char kTinyModelFilename[] = "tiny_reranker.onnx";
constexpr size_t kTinyBatchLimit = 30;
constexpr double kTinyScoreWeight = 0.15;
using Clock = std::chrono::steady_clock;

bool IsModelFile(const std::filesystem::path& path) {
  std::error_code error;
  return !path.empty() && std::filesystem::is_regular_file(path, error);
}

bool ScoreBefore(const core::Candidate& left, const core::Candidate& right) {
  const bool left_finite = std::isfinite(left.score);
  const bool right_finite = std::isfinite(right.score);
  if (left_finite != right_finite) return left_finite;
  return left_finite && left.score > right.score;
}

}  // namespace

TinyRerankerBatch PrepareTinyRerankerBatch(const std::vector<core::Candidate>& candidates,
                                           std::span<const UserSignals> user) {
  if (!user.empty() && user.size() != candidates.size()) return {};

  TinyRerankerBatch batch;
  batch.indices.resize(candidates.size());
  std::iota(batch.indices.begin(), batch.indices.end(), size_t{0});
  std::stable_sort(batch.indices.begin(), batch.indices.end(), [&](size_t left, size_t right) {
    return ScoreBefore(candidates[left], candidates[right]);
  });
  batch.indices.resize((std::min)(batch.indices.size(), kTinyBatchLimit));
  batch.features.reserve(batch.indices.size());
  size_t valid_count = 0;
  for (size_t index : batch.indices) {
    const auto& candidate = candidates[index];
    if (!std::isfinite(candidate.score)) continue;
    auto signals = candidate.rerank_signals;
    if (signals.candidate_length == 0) {
      signals.candidate_length = core::ComputeCandidateLength(candidate.surface);
    }
    const auto features = ExtractFeaturesV1(signals, user.empty() ? UserSignals{} : user[index]);
    if (!std::all_of(features.begin(), features.end(), [](float v) { return std::isfinite(v); }))
      continue;
    batch.indices[valid_count++] = index;
    batch.features.push_back(features);
  }
  batch.indices.resize(valid_count);
  return batch;
}

TinyRerankerConfig ClampTinyRerankerConfig(TinyRerankerConfig config) {
  config.timeout_ms = std::clamp(config.timeout_ms, kTinyTimeoutMinMs, kTinyTimeoutMaxMs);
  config.failure_threshold = std::clamp(config.failure_threshold, kTinyFailureMin, kTinyFailureMax);
  return config;
}

std::optional<std::filesystem::path> ResolveTinyRerankerModelPath(
    const std::string& explicit_path, const std::filesystem::path& user_models,
    const std::filesystem::path& bundled_models) {
  if (!explicit_path.empty()) {
    if (explicit_path.find('\0') != std::string::npos) return std::nullopt;
    auto path = core::Utf8Path(explicit_path);
    if (!path.is_absolute()) return std::nullopt;
    return IsModelFile(path) ? std::optional{std::move(path)} : std::nullopt;
  }
  for (const auto* directory : {&user_models, &bundled_models}) {
    if (directory->empty()) continue;
    auto path = *directory / kTinyModelFilename;
    if (IsModelFile(path)) return path;
  }
  return std::nullopt;
}

const char* TinyRerankReason(TinyRerankOutcome outcome) {
  switch (outcome) {
    case TinyRerankOutcome::Disabled:
      return "disabled";
    case TinyRerankOutcome::SecureMode:
      return "secure_mode";
    case TinyRerankOutcome::LoadFailed:
      return "load_failed";
    case TinyRerankOutcome::InvalidInput:
      return "invalid_input";
    case TinyRerankOutcome::Timeout:
      return "timeout";
    case TinyRerankOutcome::InferError:
      return "infer_error";
    case TinyRerankOutcome::CircuitOpen:
      return "circuit_open";
    case TinyRerankOutcome::Applied:
    case TinyRerankOutcome::Skipped:
      return nullptr;
  }
  return nullptr;
}

void TinyRerankerRuntime::ResetSession() {
  runtime_.reset();
  load_attempted_ = false;
  failures_ = 0;
}

void TinyRerankerRuntime::SetConfig(TinyRerankerConfig config) {
  config = ClampTinyRerankerConfig(std::move(config));
  if (config_ == config) return;
  config_ = std::move(config);
  ResetSession();
}

void TinyRerankerRuntime::SetModelDirectories(std::filesystem::path user_models,
                                              std::filesystem::path bundled_models) {
  if (user_models_ == user_models && bundled_models_ == bundled_models) return;
  user_models_ = std::move(user_models);
  bundled_models_ = std::move(bundled_models);
  ResetSession();
}

void TinyRerankerRuntime::SetFactoryForTests(Factory factory,
                                             std::optional<std::chrono::milliseconds> timeout) {
  factory_ = std::move(factory);
  timeout_for_tests_ = timeout;
  ResetSession();
}

void TinyRerankerRuntime::SetClockForTests(std::function<Clock::time_point()> now) {
  now_for_tests_ = std::move(now);
}

TinyRerankOutcome TinyRerankerRuntime::RecordFailure(TinyRerankOutcome outcome) {
  ++failures_;
  return outcome;
}

TinyRerankOutcome TinyRerankerRuntime::Apply(std::vector<core::Candidate>& candidates, bool secure,
                                             const std::atomic<bool>* cancel, Deadline deadline,
                                             const TinyRerankerBatch* prepared) {
  const auto outcome = ApplyBatch(candidates, secure, cancel, deadline, {}, prepared);
  if (outcome == TinyRerankOutcome::Applied) failures_ = 0;
  return outcome;
}

TinyRerankOutcome TinyRerankerRuntime::ApplyPredictionBatches(
    std::vector<core::Candidate>& dictionary, std::vector<core::Candidate>& predictions,
    bool secure, const std::atomic<bool>* cancel, std::span<const UserSignals> dictionary_user,
    std::span<const UserSignals> prediction_user) {
  using Outcome = TinyRerankOutcome;
  if (dictionary.empty() || predictions.empty() || !config_.enabled || secure) {
    const bool use_predictions = dictionary.empty();
    const auto outcome = ApplyBatch(use_predictions ? predictions : dictionary, secure, cancel, {},
                                    use_predictions ? prediction_user : dictionary_user);
    if (outcome == Outcome::Applied) failures_ = 0;
    return outcome;
  }

  // Keep the original scores and failure count until both batches finish.
  auto ranked_dictionary = dictionary;
  const auto first = ApplyBatch(ranked_dictionary, secure, cancel, {}, dictionary_user);
  if (first != Outcome::Applied && first != Outcome::InvalidInput) return first;
  const auto second = ApplyBatch(predictions, secure, cancel, {}, prediction_user);
  if (second != Outcome::Applied && second != Outcome::InvalidInput) return second;
  if (first == Outcome::Applied) dictionary = std::move(ranked_dictionary);
  if (first == Outcome::Applied || second == Outcome::Applied) {
    failures_ = 0;
    return Outcome::Applied;
  }
  return Outcome::InvalidInput;
}

TinyRerankOutcome TinyRerankerRuntime::ApplyBatch(std::vector<core::Candidate>& candidates,
                                                  bool secure, const std::atomic<bool>* cancel,
                                                  Deadline deadline,
                                                  std::span<const UserSignals> user,
                                                  const TinyRerankerBatch* prepared) {
  using Outcome = TinyRerankOutcome;
  using namespace std::chrono_literals;
  const auto canceled = [cancel] { return cancel && cancel->load(std::memory_order_relaxed); };
  if (canceled() || candidates.empty()) return Outcome::Skipped;
  if (!config_.enabled) return Outcome::Disabled;
  if (secure) return Outcome::SecureMode;
  if (failures_ >= config_.failure_threshold) return Outcome::CircuitOpen;
  const auto now = [this] { return now_for_tests_ ? now_for_tests_() : Clock::now(); };
  const auto timeout = timeout_for_tests_.value_or(std::chrono::milliseconds(config_.timeout_ms));
  if (deadline && *deadline - now() < timeout) return Outcome::Skipped;

  if (!load_attempted_) {
    load_attempted_ = true;
    try {
      if (factory_) {
        const auto path =
            ResolveTinyRerankerModelPath(config_.model_path, user_models_, bundled_models_);
        if (path) runtime_ = factory_(*path);
      }
    } catch (...) {
      return Outcome::LoadFailed;
    }
  }
  if (!runtime_) return Outcome::LoadFailed;

  auto local_batch = prepared ? TinyRerankerBatch{} : PrepareTinyRerankerBatch(candidates, user);
  const auto& batch = prepared ? *prepared : local_batch;
  const size_t valid_count = batch.features.size();
  if (valid_count == 0 || batch.indices.size() != valid_count ||
      (prepared && std::any_of(batch.indices.begin(), batch.indices.end(),
                               [&](size_t index) { return index >= candidates.size(); }))) {
    return Outcome::InvalidInput;
  }
  const auto started = now();
  if (canceled() || (deadline && started >= *deadline)) return Outcome::Skipped;
  if (pending_.valid()) {
    if (pending_.wait_for(0ms) != std::future_status::ready) {
      if (!pending_timed_out_ && started - pending_started_ < timeout) return Outcome::Skipped;
      pending_timed_out_ = true;
      return RecordFailure(Outcome::Timeout);
    }
    // The previous request already fell back. Discard its result.
    pending_ = {};
  }

  const auto tiny_expires = started + timeout;
  const bool request_expires_first = deadline && *deadline <= tiny_expires;
  const auto expires = request_expires_first ? *deadline : tiny_expires;
  auto stop = std::make_shared<std::atomic<bool>>(false);
  const auto request_stop = [&] {
    // No notify_all(): std::atomic wait/notify imports msvcp140_atomic_wait.dll,
    // which the MSI does not ship. Runtimes poll the flag instead.
    stop->store(true, std::memory_order_relaxed);
  };
  const auto expire = [&] {
    request_stop();
    if (request_expires_first) return Outcome::Skipped;
    pending_timed_out_ = true;
    return RecordFailure(Outcome::Timeout);
  };
  std::vector<float> scores;
  try {
    // A pending forward may outlive the caller's prepared batch.
    auto features = prepared ? prepared->features : std::move(local_batch.features);
    std::packaged_task<std::vector<float>()> work(
        [runtime = runtime_, features = std::move(features), stop] {
          return runtime->Infer(features, *stop);
        });
    auto result = work.get_future();
    std::thread worker(std::move(work));
    worker.detach();
    pending_ = std::move(result);
    pending_started_ = started;
    pending_timed_out_ = false;
    for (;;) {
      if (canceled()) {
        request_stop();
        return Outcome::Skipped;
      }
      const auto current = now();
      if (current >= expires) return expire();
      const auto next_check = cancel ? (std::min)(expires, current + 1ms) : expires;
      if (pending_.wait_until(next_check) == std::future_status::ready) break;
    }
    if (canceled()) return Outcome::Skipped;
    if (now() >= expires) return expire();
    scores = pending_.get();
  } catch (...) {
    return RecordFailure(Outcome::InferError);
  }
  if (scores.size() != valid_count ||
      !std::all_of(scores.begin(), scores.end(), [](float v) { return std::isfinite(v); })) {
    return RecordFailure(Outcome::InferError);
  }
  for (size_t i = 0; i < valid_count; ++i) {
    candidates[batch.indices[i]].score += kTinyScoreWeight * std::clamp(scores[i], 0.0f, 1.0f);
  }
  std::stable_sort(candidates.begin(), candidates.end(), ScoreBefore);
  return Outcome::Applied;
}

}  // namespace azookey::reranker
