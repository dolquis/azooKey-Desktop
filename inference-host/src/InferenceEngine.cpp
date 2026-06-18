#include "azookey/host/InferenceEngine.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <utility>

#include "azookey/host/ZenzaiModelConverter.h"

namespace azookey::host {

namespace {
constexpr const char* kLearningSaveError = "failed to save learning store";

core::ConversionContext BuildContext(const std::string& kana, const std::string& context) {
  core::ConversionContext conversion_context;
  conversion_context.preceding_text = context;
  conversion_context.preedit_text = kana;
  return conversion_context;
}
}  // namespace

InferenceEngine::InferenceEngine(std::unique_ptr<core::IConverter> converter,
                                 learning::LearningStore* store, EngineConfig config)
    : fallback_converter_(std::move(converter)),
      active_converter_(fallback_converter_.get()),
      store_(store),
      reranker_(store),
      config_(std::move(config)) {
  if (store_) {
    learning_flush_thread_ = std::thread(&InferenceEngine::LearningFlushWorker, this);
  }
}

InferenceEngine::~InferenceEngine() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    learning_flush_stop_ = true;
  }
  learning_flush_cv_.notify_all();
  if (learning_flush_thread_.joinable()) {
    learning_flush_thread_.join();
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  (void)FlushLearningStoreLocked();
}

void InferenceEngine::SetUserDictionary(learning::UserDictionary* dict) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  user_dict_ = dict;
}

std::vector<core::Candidate> InferenceEngine::ApplyRerankerOrRaw(
    const std::string& kana, std::vector<core::Candidate> candidates, uint64_t now_epoch_sec) {
  try {
    // Do not move candidates into Apply: fallback needs the raw order/scores.
    return reranker_.Apply(kana, candidates, now_epoch_sec);
  } catch (const std::exception& ex) {
    last_error_ = std::string("reranker failed: ") + ex.what();
    return candidates;
  } catch (...) {
    last_error_ = "reranker failed: unknown exception";
    return candidates;
  }
}

bool InferenceEngine::LoadModel() { return LoadModelWithResult().ok; }

bool InferenceEngine::LoadModel(const ModelLoadOptions& options) {
  return LoadModelWithResult(options).ok;
}

ModelLoadResult InferenceEngine::LoadModelWithResult() {
  const auto current = config();
  return LoadModelWithResult(
      ModelLoadOptions{current.model_path, current.backend, current.n_gpu_layers});
}

ModelLoadResult InferenceEngine::LoadModelWithResult(const ModelLoadOptions& options) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  (void)FlushLearningStoreLocked();
  ModelLoadResult result;
  EngineConfig next_config = config_;
  next_config.model_path = options.path;
  next_config.backend = options.backend;
  next_config.n_gpu_layers = options.n_gpu_layers;

  if (next_config.model_path.empty()) {
    // No model path means the MVP converter remains active as the fallback.
    config_ = std::move(next_config);
    model_loaded_ = false;
    model_converter_.reset();
    active_converter_ = fallback_converter_.get();
    last_error_.reset();
    result.ok = true;
    return result;
  }

  auto probe = ProbeZenzaiGgufModel(next_config.model_path);
  if (!probe.ok) {
    result.error = probe.error;
    if (!model_loaded_) {
      config_ = std::move(next_config);
      model_converter_.reset();
      active_converter_ = fallback_converter_.get();
      last_error_ = result.error;
    }
    return result;
  }

  if (next_config.backend == BackendKind::Cuda) {
    next_config.backend = BackendKind::Cpu;
    result.error = "CUDA backend is not linked yet; loaded GGUF with CPU fallback";
  }
  ZenzaiRuntimeOptions runtime_options;
  runtime_options.n_gpu_layers =
      next_config.backend == BackendKind::Cuda ? options.n_gpu_layers.value_or(0) : 0;
  auto loaded = LoadZenzaiGgufModel(next_config.model_path, runtime_options);
  if (!loaded.ok) {
    result.error = loaded.error;
    if (!model_loaded_) {
      config_ = std::move(next_config);
      model_converter_.reset();
      active_converter_ = fallback_converter_.get();
      last_error_ = result.error;
    }
    return result;
  }
  auto next_converter =
      std::make_unique<ZenzaiModelConverter>(std::move(loaded), fallback_converter_.get());
  config_ = std::move(next_config);
  model_converter_ = std::move(next_converter);
  active_converter_ = model_converter_.get();
  model_loaded_ = true;
  last_error_.reset();
  result.ok = true;
  return result;
}

void InferenceEngine::ApplyConfig(const EngineConfig& config) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  config_.enable_live_conversion = config.enable_live_conversion;
  config_.learning_alpha = config.learning_alpha;
  config_.learning_flush_every_n = config.learning_flush_every_n;
  config_.learning_flush_interval_sec = config.learning_flush_interval_sec;
  config_.learning_max_records = config.learning_max_records;
  config_.learning_min_weight = config.learning_min_weight;
  config_.user_word_default_score = config.user_word_default_score;
}

BackendKind InferenceEngine::backend() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return config_.backend;
}

EngineConfig InferenceEngine::config() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return config_;
}

bool InferenceEngine::model_loaded() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return model_loaded_;
}

std::optional<std::string> InferenceEngine::last_error() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return last_error_;
}

std::vector<core::Candidate> InferenceEngine::QueryCandidates(const std::string& kana,
                                                              const std::string& context,
                                                              uint64_t now_epoch_sec) {
  return QueryCandidates(kana, context, now_epoch_sec, nullptr);
}

std::vector<core::Candidate> InferenceEngine::QueryCandidates(const std::string& kana,
                                                              const std::string& context,
                                                              uint64_t now_epoch_sec,
                                                              const std::atomic<bool>* cancel) {
  auto canceled = [cancel]() { return cancel && cancel->load(std::memory_order_relaxed); };

  if (canceled()) return {};

  // Keep converter calls under state_mutex_: LoadModelWithResult can replace
  // model_converter_ and active_converter_ under the same mutex.
  std::lock_guard<std::mutex> lock(state_mutex_);

  if (canceled()) return {};

  std::vector<core::Candidate> merged;
  if (user_dict_) {
    auto words = user_dict_->Lookup(kana);
    merged.reserve(words.size());
    for (const auto& w : words) {
      core::Candidate c;
      c.surface = w.word;
      c.reading = w.ruby;
      c.score = w.value.value_or(config_.user_word_default_score);
      c.debug_info = "user-dict";
      merged.push_back(std::move(c));
    }
  }

  if (canceled()) return {};

  auto converted = active_converter_->Convert(kana, BuildContext(kana, context));
  merged.insert(merged.end(), std::make_move_iterator(converted.begin()),
                std::make_move_iterator(converted.end()));

  if (canceled()) return {};

  return ApplyRerankerOrRaw(kana, std::move(merged), now_epoch_sec);
}

std::vector<core::Candidate> InferenceEngine::QueryPredictions(const std::string& kana,
                                                               const std::string& context,
                                                               uint64_t now_epoch_sec) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  auto candidates = active_converter_->PredictNext(kana, BuildContext(kana, context));
  return ApplyRerankerOrRaw(kana, std::move(candidates), now_epoch_sec);
}

std::vector<core::Candidate> InferenceEngine::QueryCorrections(const std::string& kana,
                                                               const std::string& context,
                                                               const std::string& rejected_surface,
                                                               uint64_t now_epoch_sec) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  auto conversion_context = BuildContext(kana, context);
  conversion_context.rejected_surfaces.push_back(rejected_surface);
  core::CorrectionHint hint;
  hint.rejected_surface = rejected_surface;
  hint.intent = "user_rejection";
  auto candidates = active_converter_->Correct(kana, hint, conversion_context);
  return ApplyRerankerOrRaw(kana, std::move(candidates), now_epoch_sec);
}

void InferenceEngine::CommitObservation(const std::string& reading, const std::string& surface,
                                        uint64_t now_epoch_sec) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (store_) {
    store_->Observe(reading, surface, config_.learning_alpha, now_epoch_sec);
    NoteLearningMutationLocked(now_epoch_sec);
  }
  active_converter_->Commit(
      core::Candidate{surface, reading, 1.0, core::CandidateSource::UserDictionary, "commit"},
      core::ConversionContext{});
}

void InferenceEngine::CommitCorrection(const std::string& reading,
                                       const std::string& rejected_surface,
                                       const std::string& selected_surface,
                                       uint64_t now_epoch_sec) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (store_) {
    store_->ObserveCorrection(reading, rejected_surface, selected_surface, config_.learning_alpha,
                              now_epoch_sec);
    NoteLearningMutationLocked(now_epoch_sec);
  }

  core::ConversionContext context;
  context.rejected_surfaces.push_back(rejected_surface);
  active_converter_->Commit(
      core::Candidate{selected_surface, reading, 1.0, core::CandidateSource::UserDictionary,
                      "correction-commit"},
      context);
}

bool InferenceEngine::FlushLearningStore() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return FlushLearningStoreLocked();
}

void InferenceEngine::NoteLearningMutationLocked(uint64_t now_epoch_sec) {
  if (!store_) {
    return;
  }

  store_->Prune(config_.learning_max_records, config_.learning_min_weight, now_epoch_sec);
  ++unsaved_observations_;
  if (!first_unsaved_observation_epoch_sec_.has_value()) {
    first_unsaved_observation_epoch_sec_ = now_epoch_sec;
    first_unsaved_observation_steady_ = std::chrono::steady_clock::now();
  }
  learning_flush_cv_.notify_all();

  if (ShouldFlushLearningStoreLocked(now_epoch_sec)) {
    (void)FlushLearningStoreLocked();
  }
}

bool InferenceEngine::ShouldFlushLearningStoreLocked(uint64_t now_epoch_sec) const {
  if (!store_ || !store_->dirty() || unsaved_observations_ == 0) {
    return false;
  }
  if (config_.learning_flush_every_n > 0 &&
      unsaved_observations_ >= config_.learning_flush_every_n) {
    return true;
  }
  if (config_.learning_flush_interval_sec == 0) {
    return true;
  }
  return first_unsaved_observation_epoch_sec_.has_value() &&
         now_epoch_sec >= *first_unsaved_observation_epoch_sec_ &&
         now_epoch_sec - *first_unsaved_observation_epoch_sec_ >=
             config_.learning_flush_interval_sec;
}

bool InferenceEngine::FlushLearningStoreLocked() {
  if (!store_) {
    return true;
  }
  if (!store_->dirty()) {
    unsaved_observations_ = 0;
    first_unsaved_observation_epoch_sec_.reset();
    first_unsaved_observation_steady_.reset();
    return true;
  }
  if (!store_->Save()) {
    RecordLearningSaveFailureLocked();
    first_unsaved_observation_steady_ = std::chrono::steady_clock::now();
    return false;
  }
  unsaved_observations_ = 0;
  first_unsaved_observation_epoch_sec_.reset();
  first_unsaved_observation_steady_.reset();
  return true;
}

void InferenceEngine::RecordLearningSaveFailureLocked() {
  last_error_ = kLearningSaveError;
  std::cerr << "error: " << kLearningSaveError << std::endl;
}

void InferenceEngine::LearningFlushWorker() {
  std::unique_lock<std::mutex> lock(state_mutex_);
  while (!learning_flush_stop_) {
    if (!store_ || !store_->dirty() || config_.learning_flush_interval_sec == 0 ||
        !first_unsaved_observation_steady_.has_value()) {
      learning_flush_cv_.wait(lock, [this]() {
        return learning_flush_stop_ ||
               (store_ && store_->dirty() && config_.learning_flush_interval_sec > 0 &&
                first_unsaved_observation_steady_.has_value());
      });
      continue;
    }

    const auto deadline = *first_unsaved_observation_steady_ +
                          std::chrono::seconds(config_.learning_flush_interval_sec);
    const bool changed = learning_flush_cv_.wait_until(lock, deadline, [this]() {
      return learning_flush_stop_ || !store_ || !store_->dirty() ||
             config_.learning_flush_interval_sec == 0 ||
             !first_unsaved_observation_steady_.has_value();
    });
    if (changed) {
      continue;
    }

    (void)FlushLearningStoreLocked();
  }
}
}  // namespace azookey::host
