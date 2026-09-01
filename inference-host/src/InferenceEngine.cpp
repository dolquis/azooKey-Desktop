#include "azookey/host/InferenceEngine.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <utility>

#include "azookey/host/ZenzaiModelConverter.h"
#include "azookey/learning/FileLock.h"

namespace azookey::host {

namespace {
constexpr const char* kLearningSaveError = "failed to save learning store";
constexpr const char* kUserDictionaryLoadError = "failed to load user dictionary";
constexpr const char* kUserDictionaryLockError = "failed to lock user dictionary";
constexpr const char* kUserDictionarySaveError = "failed to save user dictionary";
constexpr auto kModelConversionBudget = std::chrono::milliseconds(600);

core::ConversionContext BuildContext(
    const std::string& kana, const std::string& context, const std::atomic<bool>* cancel = nullptr,
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt,
    uint32_t max_candidates = 0, bool live = false) {
  core::ConversionContext conversion_context;
  conversion_context.preceding_text = context;
  conversion_context.preedit_text = kana;
  conversion_context.cancel = cancel;
  conversion_context.deadline = deadline;
  conversion_context.max_candidates = max_candidates;
  conversion_context.live = live;
  return conversion_context;
}

bool PreferMergedCandidate(const core::Candidate& candidate, const core::Candidate& existing) {
  const bool candidate_is_user = candidate.source == core::CandidateSource::UserDictionary;
  const bool existing_is_user = existing.source == core::CandidateSource::UserDictionary;
  if (candidate_is_user != existing_is_user) {
    return candidate_is_user;
  }
  return candidate.score > existing.score;
}

void AppendDebugTag(std::string& debug_info, const std::string& tag) {
  if (debug_info.empty()) {
    debug_info = tag;
    return;
  }
  debug_info += ";" + tag;
}

void DedupMergedCandidates(std::vector<core::Candidate>& candidates) {
  std::vector<core::Candidate> deduped;
  deduped.reserve(candidates.size());
  for (auto& candidate : candidates) {
    auto existing = std::find_if(deduped.begin(), deduped.end(), [&](const auto& item) {
      return item.surface == candidate.surface;
    });
    if (existing == deduped.end()) {
      deduped.push_back(std::move(candidate));
      continue;
    }

    const bool replace = PreferMergedCandidate(candidate, *existing);
    if (replace) {
      AppendDebugTag(candidate.debug_info, "dup:" + existing->debug_info);
      *existing = std::move(candidate);
    } else {
      AppendDebugTag(existing->debug_info, "dup:" + candidate.debug_info);
    }
  }
  candidates = std::move(deduped);
}

void ApplyModelConfigFields(EngineConfig& target, const EngineConfig& source) {
  target.model_path = source.model_path;
  target.backend = source.backend;
  target.n_gpu_layers = source.n_gpu_layers;
}

bool UserDictionaryFileExists(const learning::UserDictionary& dict) {
  std::error_code ec;
  return std::filesystem::exists(dict.path(), ec) && !ec;
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
  WaitForModelPreload();
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

bool InferenceEngine::AddUserWord(const learning::UserWord& word) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!user_dict_) {
    return false;
  }

  // Disk is the cross-process source of truth for user dictionary edits.
  auto file_lock = learning::AcquireExclusiveFileLockForPath(user_dict_->path());
  if (!file_lock) {
    RecordUserDictionaryFailureLocked(kUserDictionaryLockError);
    return false;
  }
  if (UserDictionaryFileExists(*user_dict_) && !user_dict_->Load()) {
    RecordUserDictionaryFailureLocked(kUserDictionaryLoadError);
    return false;
  }
  const auto before = user_dict_->All();
  user_dict_->Add(word);
  if (user_dict_->Save()) {
    return true;
  }

  RestoreUserDictionaryLocked(before);
  RecordUserDictionaryFailureLocked(kUserDictionarySaveError);
  return false;
}

bool InferenceEngine::RemoveUserWord(const std::string& word, const std::string& ruby) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!user_dict_) {
    return false;
  }

  auto file_lock = learning::AcquireExclusiveFileLockForPath(user_dict_->path());
  if (!file_lock) {
    RecordUserDictionaryFailureLocked(kUserDictionaryLockError);
    return false;
  }
  if (UserDictionaryFileExists(*user_dict_) && !user_dict_->Load()) {
    RecordUserDictionaryFailureLocked(kUserDictionaryLoadError);
    return false;
  }
  const auto before = user_dict_->All();
  if (!user_dict_->Remove(word, ruby)) {
    return false;
  }
  if (user_dict_->Save()) {
    return true;
  }

  RestoreUserDictionaryLocked(before);
  RecordUserDictionaryFailureLocked(kUserDictionarySaveError);
  return false;
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

bool InferenceEngine::StartModelPreload(ModelLoadOptions options) {
  if (options.path.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> thread_lock(model_preload_thread_mutex_);
  if (model_preload_thread_.joinable()) {
    model_preload_thread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    model_preload_in_progress_ = true;
  }
  try {
    model_preload_thread_ = std::thread([this, options = std::move(options)]() mutable {
      (void)LoadModelWithResult(options);
      std::lock_guard<std::mutex> lock(state_mutex_);
      model_preload_in_progress_ = false;
    });
  } catch (...) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    model_preload_in_progress_ = false;
    throw;
  }
  return true;
}

void InferenceEngine::WaitForModelPreload() {
  std::lock_guard<std::mutex> lock(model_preload_thread_mutex_);
  if (model_preload_thread_.joinable()) {
    model_preload_thread_.join();
  }
}

ModelLoadResult InferenceEngine::LoadModelWithResult() {
  const auto current = config();
  return LoadModelWithResult(
      ModelLoadOptions{current.model_path, current.backend, current.n_gpu_layers});
}

ModelLoadResult InferenceEngine::LoadModelWithResult(const ModelLoadOptions& options) {
  std::lock_guard<std::mutex> load_lock(model_load_mutex_);
  ModelLoadResult result;
  EngineConfig next_config;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    (void)FlushLearningStoreLocked();
    next_config = config_;
    next_config.model_path = options.path;
    next_config.backend = options.backend;
    next_config.n_gpu_layers = options.n_gpu_layers;
  }

  if (next_config.model_path.empty()) {
    // No model path means the MVP converter remains active as the fallback.
    std::lock_guard<std::mutex> lock(state_mutex_);
    ApplyModelConfigFields(config_, next_config);
    model_loaded_ = false;
    model_converter_.reset();
    active_converter_ = fallback_converter_.get();
    last_error_.reset();
    model_runtime_error_.reset();
    result.ok = true;
    return result;
  }

  if (options.before_probe_for_tests) {
    options.before_probe_for_tests();
  }

  auto probe = ProbeZenzaiGgufModel(next_config.model_path);
  if (!probe.ok) {
    result.error = probe.error;
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!model_loaded_) {
      ApplyModelConfigFields(config_, next_config);
      model_converter_.reset();
      active_converter_ = fallback_converter_.get();
      last_error_ = result.error;
      model_runtime_error_.reset();
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
  runtime_options.n_threads = options.n_threads;
  runtime_options.mock_candidates_for_tests = options.mock_zenzai_candidates_for_tests;
  auto loaded = LoadZenzaiGgufModel(next_config.model_path, runtime_options);
  if (!loaded.ok) {
    result.error = loaded.error;
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!model_loaded_) {
      ApplyModelConfigFields(config_, next_config);
      model_converter_.reset();
      active_converter_ = fallback_converter_.get();
      last_error_ = result.error;
      model_runtime_error_.reset();
    }
    return result;
  }
  auto next_converter =
      std::make_unique<ZenzaiModelConverter>(std::move(loaded), fallback_converter_.get());
  std::lock_guard<std::mutex> lock(state_mutex_);
  ApplyModelConfigFields(config_, next_config);
  model_converter_ = std::move(next_converter);
  active_converter_ = model_converter_.get();
  model_loaded_ = true;
  last_error_.reset();
  model_runtime_error_.reset();
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

EngineHealthSnapshot InferenceEngine::health_snapshot() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  EngineHealthSnapshot snapshot;
  snapshot.backend = config_.backend;
  snapshot.model_loaded = model_loaded_;
  snapshot.model_preload_in_progress = model_preload_in_progress_;
  snapshot.model_path = config_.model_path;
  snapshot.last_error = last_error_ ? last_error_ : model_runtime_error_;
  snapshot.learning_entries = store_ ? store_->size() : 0;
  snapshot.user_dict_entries = user_dict_ ? user_dict_->Size() : 0;
  return snapshot;
}

bool InferenceEngine::model_loaded() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return model_loaded_;
}

bool InferenceEngine::model_preload_in_progress() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return model_preload_in_progress_;
}

std::optional<std::string> InferenceEngine::last_error() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return last_error_;
}

std::optional<std::string> InferenceEngine::effective_last_error() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (last_error_) {
    return last_error_;
  }
  return model_runtime_error_;
}

std::optional<ZenzaiDecodeStats> InferenceEngine::last_zenzai_decode_stats() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  const auto* zenzai = dynamic_cast<const ZenzaiModelConverter*>(model_converter_.get());
  if (!zenzai || active_converter_ != zenzai) {
    return std::nullopt;
  }
  return zenzai->last_decode_stats();
}

void InferenceEngine::MirrorModelRuntimeErrorLocked() {
  auto* zenzai = dynamic_cast<ZenzaiModelConverter*>(model_converter_.get());
  if (zenzai && active_converter_ == zenzai) {
    model_runtime_error_ = zenzai->last_error();
  }
}

std::vector<core::Candidate> InferenceEngine::QueryCandidates(const std::string& kana,
                                                              const std::string& context,
                                                              uint64_t now_epoch_sec) {
  return QueryCandidates(kana, context, now_epoch_sec, nullptr);
}

std::vector<core::Candidate> InferenceEngine::QueryCandidates(const std::string& kana,
                                                              const std::string& context,
                                                              uint64_t now_epoch_sec,
                                                              const std::atomic<bool>* cancel,
                                                              uint32_t max_candidates, bool live) {
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
      c.source = core::CandidateSource::UserDictionary;
      c.debug_info = "user-dict";
      merged.push_back(std::move(c));
    }
  }

  if (canceled()) return {};

  std::vector<core::Candidate> converted;
  const bool using_model_converter =
      model_converter_ && active_converter_ == model_converter_.get();
  const auto conversion_deadline = std::chrono::steady_clock::now() + kModelConversionBudget;
  try {
    converted = active_converter_->Convert(
        kana, BuildContext(kana, context, cancel, conversion_deadline, max_candidates, live));
    if (canceled()) return {};
    if (using_model_converter) {
      MirrorModelRuntimeErrorLocked();
    }
  } catch (const std::exception& ex) {
    if (!using_model_converter || !fallback_converter_) {
      throw;
    }
    if (canceled()) return {};
    model_runtime_error_ = std::string("zenzai-convert-exception:") + ex.what();
    converted = fallback_converter_->Convert(kana, BuildContext(kana, context));
  } catch (...) {
    if (!using_model_converter || !fallback_converter_) {
      throw;
    }
    if (canceled()) return {};
    model_runtime_error_ = "zenzai-convert-exception:unknown";
    converted = fallback_converter_->Convert(kana, BuildContext(kana, context));
  }
  merged.insert(merged.end(), std::make_move_iterator(converted.begin()),
                std::make_move_iterator(converted.end()));
  DedupMergedCandidates(merged);

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

  const bool starts_new_burst = unsaved_observations_ == 0;
  const auto mutation_steady = std::chrono::steady_clock::now();
  const bool burst_start_flush_due =
      starts_new_burst && (!last_successful_learning_save_steady_.has_value() ||
                           mutation_steady - *last_successful_learning_save_steady_ >=
                               std::chrono::seconds(config_.learning_flush_interval_sec));

  ++unsaved_observations_;
  if (!first_unsaved_observation_epoch_sec_.has_value()) {
    first_unsaved_observation_epoch_sec_ = now_epoch_sec;
    first_unsaved_observation_steady_ = mutation_steady;
  }
  last_unsaved_observation_epoch_sec_ = now_epoch_sec;
  learning_flush_cv_.notify_all();

  if (burst_start_flush_due || ShouldFlushLearningStoreLocked(now_epoch_sec)) {
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
    last_unsaved_observation_epoch_sec_.reset();
    first_unsaved_observation_steady_.reset();
    return true;
  }
  if (last_unsaved_observation_epoch_sec_.has_value() ||
      first_unsaved_observation_epoch_sec_.has_value()) {
    const uint64_t prune_epoch_sec = last_unsaved_observation_epoch_sec_.value_or(
        first_unsaved_observation_epoch_sec_.value_or(0));
    store_->Prune(config_.learning_max_records, config_.learning_min_weight, prune_epoch_sec);
  }
  if (!store_->Save()) {
    RecordLearningSaveFailureLocked();
    first_unsaved_observation_steady_ = std::chrono::steady_clock::now();
    return false;
  }
  last_successful_learning_save_steady_ = std::chrono::steady_clock::now();
  unsaved_observations_ = 0;
  first_unsaved_observation_epoch_sec_.reset();
  last_unsaved_observation_epoch_sec_.reset();
  first_unsaved_observation_steady_.reset();
  return true;
}

void InferenceEngine::RestoreUserDictionaryLocked(const std::vector<learning::UserWord>& entries) {
  if (!user_dict_) {
    return;
  }
  user_dict_->Clear();
  for (const auto& entry : entries) {
    user_dict_->Add(entry);
  }
}

void InferenceEngine::RecordLearningSaveFailureLocked() {
  last_error_ = kLearningSaveError;
  std::cerr << "error: " << kLearningSaveError << std::endl;
}

void InferenceEngine::RecordUserDictionaryFailureLocked(const char* error) {
  last_error_ = error;
  std::cerr << "error: " << error << std::endl;
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
