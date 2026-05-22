#include "azookey/host/InferenceEngine.h"

#include "azookey/host/ZenzaiModelConverter.h"

namespace azookey::host {

namespace {
core::ConversionContext BuildContext(const std::string& kana, const std::string& context) {
  core::ConversionContext conversion_context;
  conversion_context.preceding_text = context;
  conversion_context.preedit_text = kana;
  return conversion_context;
}
}  // namespace

InferenceEngine::InferenceEngine(std::unique_ptr<core::IConverter> converter,
                                 learning::LearningStore* store,
                                 EngineConfig config)
    : fallback_converter_(std::move(converter)),
      active_converter_(fallback_converter_.get()),
      store_(store),
      reranker_(store),
      config_(std::move(config)) {}

void InferenceEngine::SetUserDictionary(learning::UserDictionary* dict) {
  user_dict_ = dict;
}

bool InferenceEngine::LoadModel() {
  return LoadModelWithResult().ok;
}

bool InferenceEngine::LoadModel(const ModelLoadOptions& options) {
  return LoadModelWithResult(options).ok;
}

ModelLoadResult InferenceEngine::LoadModelWithResult() {
  const auto current = config();
  return LoadModelWithResult(ModelLoadOptions{current.model_path, current.backend, current.n_gpu_layers});
}

ModelLoadResult InferenceEngine::LoadModelWithResult(const ModelLoadOptions& options) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  ModelLoadResult result;
  config_.model_path = options.path;
  config_.backend = options.backend;
  config_.n_gpu_layers = options.n_gpu_layers;
  model_loaded_ = false;
  model_converter_.reset();
  active_converter_ = fallback_converter_.get();
  last_error_.reset();

  if (config_.model_path.empty()) {
    // No model path means the MVP converter remains active as the fallback.
    result.ok = true;
    return result;
  }

  auto probe = ProbeZenzaiGgufModel(config_.model_path);
  if (!probe.ok) {
    result.error = probe.error;
    last_error_ = result.error;
    return result;
  }

  if (config_.backend == BackendKind::Cuda) {
    config_.backend = BackendKind::Cpu;
    last_error_ = "CUDA backend is not linked yet; loaded GGUF with CPU fallback";
  }
  model_converter_ = std::make_unique<ZenzaiModelConverter>(
      std::move(probe.info), fallback_converter_.get());
  active_converter_ = model_converter_.get();
  model_loaded_ = true;
  result.ok = true;
  result.error = last_error_;
  return result;
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
  auto canceled = [cancel]() {
    return cancel && cancel->load(std::memory_order_relaxed);
  };

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
  merged.insert(merged.end(),
                std::make_move_iterator(converted.begin()),
                std::make_move_iterator(converted.end()));

  if (canceled()) return {};

  return reranker_.Apply(kana, std::move(merged), now_epoch_sec);
}

std::vector<core::Candidate> InferenceEngine::QueryPredictions(const std::string& kana,
                                                               const std::string& context,
                                                               uint64_t now_epoch_sec) {
  auto candidates = active_converter_->PredictNext(kana, BuildContext(kana, context));
  return reranker_.Apply(kana, std::move(candidates), now_epoch_sec);
}

std::vector<core::Candidate> InferenceEngine::QueryCorrections(const std::string& kana,
                                                               const std::string& context,
                                                               const std::string& rejected_surface,
                                                               uint64_t now_epoch_sec) {
  auto conversion_context = BuildContext(kana, context);
  conversion_context.rejected_surfaces.push_back(rejected_surface);
  core::CorrectionHint hint;
  hint.rejected_surface = rejected_surface;
  hint.intent = "user_rejection";
  auto candidates = active_converter_->Correct(kana, hint, conversion_context);
  return reranker_.Apply(kana, std::move(candidates), now_epoch_sec);
}

void InferenceEngine::CommitObservation(const std::string& reading, const std::string& surface, uint64_t now_epoch_sec) {
  if (store_) {
    store_->Observe(reading, surface, config_.learning_alpha, now_epoch_sec);
    store_->Save();
  }
  active_converter_->Commit(core::Candidate{surface, reading, 1.0, core::CandidateSource::UserDictionary, "commit"},
                            core::ConversionContext{});
}


void InferenceEngine::CommitCorrection(const std::string& reading,
                                       const std::string& rejected_surface,
                                       const std::string& selected_surface,
                                       uint64_t now_epoch_sec) {
  if (store_) {
    store_->ObserveCorrection(reading, rejected_surface, selected_surface, config_.learning_alpha, now_epoch_sec);
    store_->Save();
  }

  core::ConversionContext context;
  context.rejected_surfaces.push_back(rejected_surface);
  active_converter_->Commit(core::Candidate{selected_surface, reading, 1.0, core::CandidateSource::UserDictionary, "correction-commit"},
                            context);
}

}  // namespace azookey::host
