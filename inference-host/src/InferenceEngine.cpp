#include "azookey/host/InferenceEngine.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <utility>

#include "azookey/core/Utf8.h"
#include "azookey/host/DictionaryCandidateProvider.h"
#include "azookey/host/ZenzaiModelConverter.h"
#include "azookey/learning/FileLock.h"

namespace azookey::host {

namespace {
constexpr const char* kLearningSaveError = "failed to save learning store";
constexpr const char* kUserDictionaryLoadError = "failed to load user dictionary";
constexpr const char* kUserDictionaryLockError = "failed to lock user dictionary";
constexpr const char* kUserDictionarySaveError = "failed to save user dictionary";
constexpr auto kModelConversionBudget = std::chrono::milliseconds(600);
constexpr size_t kPredictionDisplayLimit = 5;

using core::TakeLastUtf8Codepoints;

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
  target.inference_threads = source.inference_threads;
}

bool UserDictionaryFileExists(const learning::UserDictionary& dict) {
  std::error_code ec;
  return std::filesystem::exists(dict.path(), ec) && !ec;
}
}  // namespace

InferenceEngine::InferenceEngine(std::unique_ptr<core::IConverter> converter,
                                 learning::LearningStore* store, EngineConfig config,
                                 logging::RuntimeLogger* runtime_logger)
    : fallback_converter_(std::move(converter)),
      active_converter_(fallback_converter_),
      store_(store),
      reranker_(store),
      config_(std::move(config)),
      runtime_logger_(runtime_logger) {
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
  RefreshDictionaryLocked();
}

void InferenceEngine::RefreshDictionaryLocked() {
  if (indexed_user_dict_ == user_dict_ &&
      (!user_dict_ || indexed_user_revision_ == user_dict_->revision()))
    return;
  dictionaries_.SetUserWords(user_dict_ ? user_dict_->All() : std::vector<learning::UserWord>{});
  indexed_user_dict_ = user_dict_;
  indexed_user_revision_ = user_dict_ ? user_dict_->revision() : 0;
}

bool InferenceEngine::LoadDictionaryLayer(learning::LayerId layer,
                                          const std::filesystem::path& path, bool verify) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return dictionaries_.LoadStatic(layer, path, verify);
}

std::vector<InferenceEngine::DictionaryLoadResult> InferenceEngine::LoadBundledDictionaryLayers(
    const std::filesystem::path& directory) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  std::vector<DictionaryLoadResult> results;
  for (const auto& [layer, name] :
       {std::pair{learning::LayerId::Base, "base_lexicon.azdic"},
        std::pair{learning::LayerId::Sudachi, "sudachi_lexicon.azdic"},
        std::pair{learning::LayerId::NamedEntity, "named_entity_lexicon.azdic"},
        std::pair{learning::LayerId::TechnicalTerms, "technical_terms_lexicon.azdic"}}) {
    const bool loaded = dictionaries_.LoadStatic(layer, directory / name);
    results.push_back({name, loaded, dictionaries_.LayerError(layer)});
  }
  return results;
}

void InferenceEngine::EnableDictionaryLayer(learning::LayerId layer, bool enabled) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  dictionaries_.EnableLayer(layer, enabled);
  if (layer == learning::LayerId::User) user_dictionary_enabled_ = enabled;
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
  return LoadModelWithResult(ModelLoadOptions{current.model_path, current.backend,
                                              current.n_gpu_layers, current.inference_threads});
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
    if (options.n_threads) {
      next_config.inference_threads = options.n_threads;
    }
  }

  if (next_config.model_path.empty()) {
    // No model path means the MVP converter remains active as the fallback.
    std::lock_guard<std::mutex> lock(state_mutex_);
    ApplyModelConfigFields(config_, next_config);
    model_loaded_ = false;
    model_converter_.reset();
    active_converter_ = fallback_converter_;
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
      active_converter_ = fallback_converter_;
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
  runtime_options.use_vulkan = next_config.backend == BackendKind::Vulkan;
  runtime_options.n_gpu_layers = runtime_options.use_vulkan ? options.n_gpu_layers.value_or(-1) : 0;
  runtime_options.n_threads = next_config.inference_threads;
  runtime_options.mock_candidates_for_tests = options.mock_zenzai_candidates_for_tests;
  const auto load = [&]() {
    try {
      if (options.before_load_for_tests) {
        options.before_load_for_tests(next_config.backend);
      }
      return LoadZenzaiGgufModel(next_config.model_path, runtime_options);
    } catch (const std::exception& ex) {
      ZenzaiLoadResult failed;
      failed.error = std::string("model initialization failed: ") + ex.what();
      return failed;
    } catch (...) {
      ZenzaiLoadResult failed;
      failed.error = "model initialization failed: unknown exception";
      return failed;
    }
  };
  auto loaded = load();
  std::optional<std::string> backend_error;
  if (!loaded.ok && runtime_options.use_vulkan) {
    backend_error = "Vulkan initialization failed; using CPU fallback: " + loaded.error;
    next_config.backend = BackendKind::Cpu;
    runtime_options.use_vulkan = false;
    runtime_options.n_gpu_layers = 0;
    loaded = load();
    result.error = backend_error;
  }
  if (!loaded.ok) {
    result.error =
        backend_error ? *backend_error + "; CPU fallback failed: " + loaded.error : loaded.error;
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!model_loaded_) {
      ApplyModelConfigFields(config_, next_config);
      model_converter_.reset();
      active_converter_ = fallback_converter_;
      last_error_ = result.error;
      model_runtime_error_.reset();
    }
    return result;
  }
  auto next_converter =
      std::make_shared<ZenzaiModelConverter>(std::move(loaded), fallback_converter_.get());
  std::lock_guard<std::mutex> lock(state_mutex_);
  ApplyModelConfigFields(config_, next_config);
  model_converter_ = std::move(next_converter);
  active_converter_ = model_converter_;
  model_loaded_ = true;
  last_error_ = backend_error;
  model_runtime_error_.reset();
  result.ok = true;
  return result;
}

void InferenceEngine::ApplyConfig(const EngineConfig& config) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  config_.nll = ClampNllConfig(config.nll);
  ++nll_config_revision_;
  config_.enable_live_conversion = config.enable_live_conversion;
  config_.inference_threads = config.inference_threads;
  config_.max_candidates = config.max_candidates;
  config_.max_context_length = config.max_context_length;
  config_.learning_alpha = config.learning_alpha;
  config_.learning_flush_every_n = config.learning_flush_every_n;
  config_.learning_flush_interval_sec = config.learning_flush_interval_sec;
  config_.learning_max_records = config.learning_max_records;
  config_.learning_min_weight = config.learning_min_weight;
  config_.prediction_learning_max_entries = config.prediction_learning_max_entries;
  config_.prediction_learning_min_score = config.prediction_learning_min_score;
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
  std::shared_ptr<core::IConverter> converter;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!model_converter_ || active_converter_ != model_converter_) return std::nullopt;
    converter = model_converter_;
  }
  std::lock_guard<std::mutex> converter_lock(converter_call_mutex_);
  const auto* zenzai = dynamic_cast<const ZenzaiModelConverter*>(converter.get());
  if (!zenzai) return std::nullopt;
  return zenzai->last_decode_stats();
}

void InferenceEngine::MirrorModelRuntimeErrorLocked(
    const std::shared_ptr<core::IConverter>& converter) {
  auto* zenzai = dynamic_cast<ZenzaiModelConverter*>(converter.get());
  if (zenzai && active_converter_ == converter) {
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

  std::shared_ptr<core::IConverter> converter;
  std::shared_ptr<core::IConverter> fallback_converter;
  EngineConfig config;
  uint64_t nll_revision{};
  bool using_model_converter = false;
  std::vector<core::Candidate> merged;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    converter = active_converter_;
    fallback_converter = fallback_converter_;
    config = config_;
    nll_revision = nll_config_revision_;
    using_model_converter = model_converter_ && converter == model_converter_;
    // Preserve M9's explicit user word score while static layers use M53 scoring.
    RefreshDictionaryLocked();
    merged = DictionaryCandidates(dictionaries_, kana, learning::LookupMode::Exact, now_epoch_sec,
                                  32, false);
    if (user_dict_ && user_dictionary_enabled_) {
      auto words = user_dict_->Lookup(kana);
      merged.reserve(merged.size() + words.size());
      for (const auto& w : words) {
        core::Candidate c;
        c.surface = w.word;
        c.reading = w.ruby;
        c.score = w.value.value_or(config.user_word_default_score);
        c.source = core::CandidateSource::UserDictionary;
        c.debug_info = "user-dict";
        merged.push_back(std::move(c));
      }
    }
  }

  if (canceled()) return {};

  std::vector<core::Candidate> converted;
  const auto conversion_deadline = std::chrono::steady_clock::now() + kModelConversionBudget;
  const uint32_t effective_max_candidates = config.max_candidates == 0 ? max_candidates
                                            : max_candidates == 0
                                                ? config.max_candidates
                                                : std::min(max_candidates, config.max_candidates);
  const auto limited_context = TakeLastUtf8Codepoints(context, config.max_context_length);
  std::unique_lock<std::mutex> converter_lock(converter_call_mutex_);
  {
    // IConverter implementations own mutable decode state. Serialize calls to
    // that state without holding state_mutex_, so Health and model swaps remain responsive.
    try {
      converted =
          converter->Convert(kana, BuildContext(kana, limited_context, cancel, conversion_deadline,
                                                effective_max_candidates, live));
      if (canceled()) return {};
      if (using_model_converter) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        MirrorModelRuntimeErrorLocked(converter);
      }
    } catch (const std::exception& ex) {
      if (!using_model_converter || !fallback_converter) throw;
      if (canceled()) return {};
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (active_converter_ == converter) {
          model_runtime_error_ = std::string("zenzai-convert-exception:") + ex.what();
        }
      }
      converted = fallback_converter->Convert(
          kana, BuildContext(kana, limited_context, cancel, std::nullopt, effective_max_candidates,
                             live));
    } catch (...) {
      if (!using_model_converter || !fallback_converter) throw;
      if (canceled()) return {};
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (active_converter_ == converter) {
          model_runtime_error_ = "zenzai-convert-exception:unknown";
        }
      }
      converted = fallback_converter->Convert(
          kana, BuildContext(kana, limited_context, cancel, std::nullopt, effective_max_candidates,
                             live));
    }
  }
  // Enabled NLL retains ownership through merge so another request cannot replace
  // the conversion's runtime error or KV state before its scoring step.
  if (!config.nll.enabled || live) converter_lock.unlock();
  merged.insert(merged.end(), std::make_move_iterator(converted.begin()),
                std::make_move_iterator(converted.end()));
  DedupMergedCandidates(merged);

  if (canceled()) return {};

  // The disabled/live path has no evaluation, allocation or additional logging.
  if (config.nll.enabled && !live) {
    NllOutcome outcome;
    auto* zenzai = dynamic_cast<ZenzaiModelConverter*>(converter.get());
    if (zenzai && zenzai->runtime_loaded() && !zenzai->last_error()) {
      outcome = zenzai->RerankNll(kana, merged,
                                  BuildContext(kana, limited_context, cancel, conversion_deadline,
                                               effective_max_candidates, live),
                                  config.nll, nll_revision);
      std::lock_guard<std::mutex> lock(state_mutex_);
      MirrorModelRuntimeErrorLocked(converter);
    } else {
      outcome.reason = "model_not_loaded";
      outcome.targets = std::min<size_t>(
          static_cast<size_t>(ClampNllConfig(config.nll).top_k),
          static_cast<size_t>(
              std::count_if(merged.begin(), merged.end(), [](const auto& candidate) {
                return candidate.source == core::CandidateSource::SystemDictionary ||
                       candidate.source == core::CandidateSource::Heuristic;
              })));
    }
    converter_lock.unlock();
    if (canceled()) return {};
    if (runtime_logger_ && !outcome.reason.empty()) {
      runtime_logger_->Log(logging::RuntimeLogLevel::Info, "nll_rerank",
                           {{"reason", logging::RuntimeLogSafeText(outcome.reason)},
                            {"nll_targets", static_cast<uint64_t>(outcome.targets)},
                            {"nll_applied", static_cast<uint64_t>(outcome.applied)},
                            {"prefix_ms", outcome.prefix_ms},
                            {"elapsed_ms", outcome.elapsed_ms}});
    }
  }

  std::vector<core::Candidate> result;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    result = ApplyRerankerOrRaw(kana, std::move(merged), now_epoch_sec);
  }
  if (effective_max_candidates > 0 && result.size() > effective_max_candidates) {
    result.resize(effective_max_candidates);
  }
  return result;
}

std::vector<core::Candidate> InferenceEngine::QueryPredictions(const std::string& kana,
                                                               const std::string& context,
                                                               uint64_t now_epoch_sec) {
  std::shared_ptr<core::IConverter> converter;
  uint32_t max_context_length = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    converter = active_converter_;
    max_context_length = config_.max_context_length;
  }
  std::vector<core::Candidate> candidates;
  {
    std::lock_guard<std::mutex> converter_lock(converter_call_mutex_);
    candidates = converter->PredictNext(
        kana, BuildContext(kana, TakeLastUtf8Codepoints(context, max_context_length)));
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  RefreshDictionaryLocked();
  auto dictionary_predictions =
      DictionaryCandidates(dictionaries_, kana, learning::LookupMode::PredictivePrefix,
                           now_epoch_sec, 32, true, config_.user_word_default_score);
  dictionary_predictions =
      ApplyRerankerOrRaw(kana, std::move(dictionary_predictions), now_epoch_sec);
  candidates = ApplyRerankerOrRaw(kana, std::move(candidates), now_epoch_sec);

  std::vector<core::Candidate> merged;
  merged.reserve(kPredictionDisplayLimit);
  if (store_ && config_.prediction_learning_max_entries > 0) {
    const size_t learning_limit =
        std::min(config_.prediction_learning_max_entries, kPredictionDisplayLimit - 2);
    const auto lookup = store_->LookupPrefix(kana, learning_limit,
                                             config_.prediction_learning_min_score, now_epoch_sec);
    for (const auto& match : lookup.matches) {
      if (match.reading == kana) continue;
      const bool duplicate = std::any_of(merged.begin(), merged.end(), [&](const auto& item) {
        return item.surface == match.surface;
      });
      if (!duplicate) {
        merged.push_back({match.surface, match.reading, match.score,
                          core::CandidateSource::Learning, "learning-prefix"});
      }
      if (merged.size() == kPredictionDisplayLimit) break;
    }
  }
  auto append = [&](std::vector<core::Candidate>& source, size_t limit) {
    size_t added = 0;
    for (auto& candidate : source) {
      if (merged.size() == kPredictionDisplayLimit || added == limit) break;
      if (candidate.surface.empty()) continue;
      const bool duplicate = std::any_of(merged.begin(), merged.end(), [&](const auto& item) {
        return item.surface == candidate.surface;
      });
      if (!duplicate) {
        merged.push_back(std::move(candidate));
        candidate.surface.clear();
        ++added;
      }
    }
  };
  append(candidates, 2);
  append(dictionary_predictions, 2);
  append(candidates, kPredictionDisplayLimit);
  return merged;
}

std::vector<core::Candidate> InferenceEngine::QueryCorrections(const std::string& kana,
                                                               const std::string& context,
                                                               const std::string& rejected_surface,
                                                               uint64_t now_epoch_sec) {
  std::shared_ptr<core::IConverter> converter;
  uint32_t max_context_length = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    converter = active_converter_;
    max_context_length = config_.max_context_length;
  }
  auto conversion_context = BuildContext(kana, TakeLastUtf8Codepoints(context, max_context_length));
  conversion_context.rejected_surfaces.push_back(rejected_surface);
  core::CorrectionHint hint;
  hint.rejected_surface = rejected_surface;
  hint.intent = "user_rejection";
  std::vector<core::Candidate> candidates;
  {
    std::lock_guard<std::mutex> converter_lock(converter_call_mutex_);
    candidates = converter->Correct(kana, hint, conversion_context);
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  return ApplyRerankerOrRaw(kana, std::move(candidates), now_epoch_sec);
}

void InferenceEngine::CommitObservation(const std::string& reading, const std::string& surface,
                                        uint64_t now_epoch_sec) {
  std::shared_ptr<core::IConverter> converter;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (store_) {
      store_->Observe(reading, surface, config_.learning_alpha, now_epoch_sec);
      NoteLearningMutationLocked(now_epoch_sec);
    }
    converter = active_converter_;
  }
  std::lock_guard<std::mutex> converter_lock(converter_call_mutex_);
  converter->Commit(
      core::Candidate{surface, reading, 1.0, core::CandidateSource::UserDictionary, "commit"},
      core::ConversionContext{});
}

void InferenceEngine::CommitCorrection(const std::string& reading,
                                       const std::string& rejected_surface,
                                       const std::string& selected_surface,
                                       uint64_t now_epoch_sec) {
  std::shared_ptr<core::IConverter> converter;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (store_) {
      store_->ObserveCorrection(reading, rejected_surface, selected_surface, config_.learning_alpha,
                                now_epoch_sec);
      NoteLearningMutationLocked(now_epoch_sec);
    }
    converter = active_converter_;
  }

  core::ConversionContext context;
  context.rejected_surfaces.push_back(rejected_surface);
  std::lock_guard<std::mutex> converter_lock(converter_call_mutex_);
  converter->Commit(core::Candidate{selected_surface, reading, 1.0,
                                    core::CandidateSource::UserDictionary, "correction-commit"},
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
