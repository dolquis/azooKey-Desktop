#include <algorithm>
#include <cmath>

#include "azookey/host/InferenceEngine.h"
#include "azookey/ipc/TraceId.h"
#include "azookey/learning/ContextHash.h"
#include "azookey/learning/LearningDecay.h"

namespace azookey::host {
namespace {
constexpr auto kExposureLifetime = std::chrono::minutes(5);
constexpr size_t kMaxCachedExposures = 64;
}  // namespace

bool InferenceEngine::ExposureFeature::Matches(const core::Candidate& candidate) const {
  return surface == candidate.surface && reading == candidate.reading && source == candidate.source;
}

void InferenceEngine::SetRerankerTraceStore(learning::RerankerTraceStore* store) {
  std::lock_guard lock(state_mutex_);
  exposure_trace_store_ = store;
  InvalidateExposuresLocked();
  SyncExposurePolicyLocked();
}

void InferenceEngine::SetExposureTracePolicy(bool enabled, bool learning_allowed) {
  learning::RerankerTraceStore* revoked = nullptr;
  {
    std::lock_guard lock(state_mutex_);
    if (config_.exposure_trace_enabled && exposure_learning_allowed_ &&
        (!enabled || !learning_allowed))
      revoked = exposure_trace_store_;
    config_.exposure_trace_enabled = enabled;
    exposure_learning_allowed_ = learning_allowed;
    SyncExposurePolicyLocked();
  }
  // The settings update acknowledges revocation after an already-started replacement.
  // New conversions and commit handling can proceed while that write finishes.
  if (revoked) revoked->WaitForPublication();
}

bool InferenceEngine::ExposureAllowedLocked() const {
  return exposure_trace_store_ && config_.exposure_trace_enabled && exposure_learning_allowed_ &&
         health_.state() != HealthState::SafeMode && !learning_erasure_in_progress_;
}

void InferenceEngine::SyncExposurePolicyLocked() {
  const bool allow_learning =
      exposure_learning_allowed_ && health_.state() != HealthState::SafeMode;
  if (!config_.exposure_trace_enabled || !allow_learning) InvalidateExposuresLocked();
  if (exposure_trace_store_) {
    exposure_trace_store_->SetRecording(config_.exposure_trace_enabled, allow_learning);
  }
}

void InferenceEngine::InvalidateExposuresLocked() {
  ++exposure_generation_;
  if (exposures_.empty()) return;
  exposures_.clear();
  learning_flush_cv_.notify_one();
}

void InferenceEngine::PruneExpiredExposuresLocked(std::chrono::steady_clock::time_point now) {
  while (!exposures_.empty() && exposures_.front().expires <= now) exposures_.pop_front();
}

void InferenceEngine::LogExposureSkipped(std::string_view reason) const {
  if (runtime_logger_) {
    runtime_logger_->Log(logging::RuntimeLogLevel::Info, "reranker_trace_skipped",
                         {{"reason", logging::RuntimeLogSafeText(std::string(reason))}});
  }
}

learning::RerankerTraceStats InferenceEngine::ExposureTraceStats() const {
  std::lock_guard lock(state_mutex_);
  return exposure_trace_store_ ? exposure_trace_store_->Stats() : learning::RerankerTraceStats{};
}

std::optional<uint64_t> InferenceEngine::BeginExposureCapture(bool allowed, bool secure) {
  const char* reason;
  {
    std::lock_guard lock(state_mutex_);
    if (!exposure_trace_store_) return std::nullopt;
    if (allowed && ExposureAllowedLocked()) return exposure_generation_;
    reason = secure                            ? "secure"
             : !config_.exposure_trace_enabled ? "opt_in_disabled"
                                               : "learning_disallowed";
  }
  LogExposureSkipped(reason);
  return std::nullopt;
}

void InferenceEngine::SetExposureClockForTests(
    std::function<std::chrono::steady_clock::time_point()> clock) {
  std::lock_guard lock(state_mutex_);
  exposure_clock_for_tests_ = std::move(clock);
  learning_flush_cv_.notify_one();
}

std::chrono::steady_clock::time_point InferenceEngine::ExposureNowLocked() const {
  return exposure_clock_for_tests_ ? exposure_clock_for_tests_() : std::chrono::steady_clock::now();
}

std::optional<std::string> InferenceEngine::PublishExposure(
    uint64_t generation, std::string_view client_id, std::string_view context,
    const std::vector<ipc::CandidateField>& candidates,
    std::vector<std::optional<std::array<float, 12>>> features) {
  if (features.size() != candidates.size() ||
      std::none_of(features.begin(), features.end(),
                   [](const auto& row) { return row.has_value(); }))
    return std::nullopt;

  std::unique_lock lock(state_mutex_);
  if (!ExposureAllowedLocked() || generation != exposure_generation_) return std::nullopt;
  const auto now = ExposureNowLocked();
  PruneExpiredExposuresLocked(now);
  std::string id;
  try {
    id = ipc::GenerateTraceId();
  } catch (...) {
    lock.unlock();
    LogExposureSkipped("id_generation_failed");
    return std::nullopt;
  }
  const bool evicted = exposures_.size() == kMaxCachedExposures;
  if (evicted) exposures_.pop_front();
  exposures_.push_back({id, std::string(client_id), learning::ContextHash(context), candidates,
                        std::move(features), now + kExposureLifetime});
  learning_flush_cv_.notify_one();
  lock.unlock();
  if (evicted) LogExposureSkipped("response_capacity");
  return id;
}

const char* InferenceEngine::RecordExposureLocked(const ipc::CommitObservationRequest& request,
                                                  const std::string& payload,
                                                  std::string_view client_id) {
  if (!ExposureAllowedLocked() || !request.exposure_id) return nullptr;
  if (request.observation_id.empty()) return "missing_observation_id";
  PruneExpiredExposuresLocked(ExposureNowLocked());
  const auto response = std::find_if(exposures_.begin(), exposures_.end(), [&](const auto& item) {
    return item.id == *request.exposure_id && item.client_id == client_id;
  });
  if (response == exposures_.end()) return "response_unavailable";
  const auto parsed = ipc::ParseCommitObservationRequest(payload, response->candidates.size());
  if (!parsed || !parsed->shown_at_ms || !parsed->shown_candidate_indices) return "invalid_display";
  learning::RerankerExposure record;
  record.observation_id = request.observation_id;
  record.left_context_hash = response->left_context_hash;
  record.reading = request.reading;
  record.chosen_surface = request.chosen.surface;
  record.shown_at_ms = *parsed->shown_at_ms;
  record.commit_ts = request.timestamp_ms;
  bool shown_chosen = false;
  for (const auto index : *parsed->shown_candidate_indices) {
    if (!response->features[index]) continue;
    const auto& candidate = response->candidates[index];
    if (candidate.surface == request.chosen.surface && candidate.reading == request.chosen.reading)
      shown_chosen = true;
    record.shown_surfaces.push_back(candidate.surface);
    record.features.push_back(*response->features[index]);
  }
  if (!shown_chosen) return "chosen_not_exposed";
  return exposure_trace_store_->Enqueue(std::move(record)) ? nullptr : "commit_not_queued";
}

const char* InferenceEngine::RecordExposureCorrectionLocked(
    const ipc::CommitCorrectionRequest& request) {
  if (!ExposureAllowedLocked() || !request.target_observation_id) return nullptr;
  if (request.observation_id.empty() || request.target_observation_id->empty())
    return "missing_observation_id";
  const bool queued = exposure_trace_store_->EnqueueCorrection(
      request.observation_id, *request.target_observation_id, request.selected_surface,
      request.timestamp_ms);
  return queued ? nullptr : "correction_not_queued";
}

reranker::UserSignals InferenceEngine::TinyUserSignalsLocked(const std::string& reading,
                                                             const std::string& surface,
                                                             uint16_t category_mask,
                                                             uint64_t now_epoch_sec) const {
  const auto* rows = store_ ? store_->Rows(reading, surface) : nullptr;
  if (!rows) return {};
  const double half_life = learning::HalfLifeDaysForCategoryMask(category_mask);
  double frequency = 0.0;
  double weighted_recency = 0.0;
  for (const auto& [app, row] : *rows) {
    const double weight = std::log1p(static_cast<double>(row.commit_count));
    frequency += weight;
    weighted_recency +=
        weight * learning::HalfLifeRecency(row.last_updated_epoch_sec, now_epoch_sec, half_life);
  }
  return frequency > 0 ? reranker::UserSignals{frequency, weighted_recency / frequency}
                       : reranker::UserSignals{};
}

std::vector<reranker::UserSignals> InferenceEngine::TinyUserSignalsForCandidatesLocked(
    const std::vector<core::Candidate>& candidates, uint64_t now_epoch_sec,
    const DictionaryCategoryMasks& categories) const {
  if (!store_) return {};
  std::vector<reranker::UserSignals> user;
  user.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    const auto mask = categories.find({candidate.reading, candidate.surface});
    user.push_back(TinyUserSignalsLocked(candidate.reading, candidate.surface,
                                         mask == categories.end() ? 0 : mask->second,
                                         now_epoch_sec));
  }
  return user;
}

}  // namespace azookey::host
