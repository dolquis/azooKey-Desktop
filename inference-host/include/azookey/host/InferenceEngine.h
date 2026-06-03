#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "azookey/core/IConverter.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/Reranker.h"
#include "azookey/learning/UserDictionary.h"

namespace azookey::host {

enum class BackendKind {
  Cpu,
  Cuda,
};

struct EngineConfig {
  BackendKind backend{BackendKind::Cpu};
  std::string model_path;
  std::optional<int32_t> n_gpu_layers;
  bool enable_live_conversion{true};
  double learning_alpha{0.8};
  size_t learning_flush_every_n{8};
  uint64_t learning_flush_interval_sec{5};
  size_t learning_max_records{10000};
  double learning_min_weight{0.05};
  // Default score for user-dictionary entries that lack an explicit value.
  double user_word_default_score{1.5};
};

struct ModelLoadOptions {
  std::string path;
  BackendKind backend{BackendKind::Cpu};
  std::optional<int32_t> n_gpu_layers;
};

struct ModelLoadResult {
  bool ok{false};
  std::optional<std::string> error;
};

class InferenceEngine {
 public:
  InferenceEngine(std::unique_ptr<core::IConverter> converter,
                  learning::LearningStore* store, EngineConfig config);
  ~InferenceEngine();

  // External, non-owning. May be nullptr (no user dictionary).
  void SetUserDictionary(learning::UserDictionary* dict);

  bool LoadModel();
  bool LoadModel(const ModelLoadOptions& options);
  ModelLoadResult LoadModelWithResult();
  ModelLoadResult LoadModelWithResult(const ModelLoadOptions& options);

  // QueryCandidates with optional cancel polling. Returns an empty vector
  // immediately when *cancel is observed true. cancel may be nullptr.
  std::vector<core::Candidate> QueryCandidates(const std::string& kana,
                                                const std::string& context,
                                                uint64_t now_epoch_sec,
                                                const std::atomic<bool>* cancel);

  // Backwards-compatible overload without cancel support.
  std::vector<core::Candidate> QueryCandidates(const std::string& kana,
                                                const std::string& context,
                                                uint64_t now_epoch_sec);

  std::vector<core::Candidate> QueryPredictions(const std::string& kana, const std::string& context, uint64_t now_epoch_sec);
  std::vector<core::Candidate> QueryCorrections(const std::string& kana,
                                                const std::string& context,
                                                const std::string& rejected_surface,
                                                uint64_t now_epoch_sec);
  void CommitObservation(const std::string& reading, const std::string& surface, uint64_t now_epoch_sec);
  void CommitCorrection(const std::string& reading,
                        const std::string& rejected_surface,
                        const std::string& selected_surface,
                        uint64_t now_epoch_sec);
  bool FlushLearningStore();

  BackendKind backend() const;
  EngineConfig config() const;
  bool model_loaded() const;
  std::optional<std::string> last_error() const;

 private:
  void NoteLearningMutationLocked(uint64_t now_epoch_sec);
  bool ShouldFlushLearningStoreLocked(uint64_t now_epoch_sec) const;
  bool FlushLearningStoreLocked();
  void RecordLearningSaveFailureLocked();
  void LearningFlushWorker();

  std::unique_ptr<core::IConverter> fallback_converter_;
  std::unique_ptr<core::IConverter> model_converter_;
  core::IConverter* active_converter_{nullptr};
  learning::LearningStore* store_;
  learning::Reranker reranker_;
  learning::UserDictionary* user_dict_{nullptr};
  mutable std::mutex state_mutex_;
  EngineConfig config_;
  bool model_loaded_{false};
  std::optional<std::string> last_error_;
  size_t unsaved_observations_{0};
  std::optional<uint64_t> first_unsaved_observation_epoch_sec_;
  std::optional<std::chrono::steady_clock::time_point> first_unsaved_observation_steady_;
  std::condition_variable learning_flush_cv_;
  std::thread learning_flush_thread_;
  bool learning_flush_stop_{false};
};

}  // namespace azookey::host
