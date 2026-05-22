#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

  BackendKind backend() const;
  EngineConfig config() const;
  bool model_loaded() const;
  std::optional<std::string> last_error() const;

 private:
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
};

}  // namespace azookey::host
