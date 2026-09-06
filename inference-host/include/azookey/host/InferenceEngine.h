#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "azookey/core/IConverter.h"
#include "azookey/host/ZenzaiDecodeStats.h"
#include "azookey/learning/DictionaryStore.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/Reranker.h"
#include "azookey/learning/UserDictionary.h"

namespace azookey::host {

enum class BackendKind {
  Cpu,
  Cuda,
  Vulkan,
};

constexpr const char* BackendName(BackendKind backend) {
  switch (backend) {
    case BackendKind::Cpu:
      return "cpu";
    case BackendKind::Cuda:
      return "cuda";
    case BackendKind::Vulkan:
      return "vulkan";
  }
  return "cpu";
}

struct EngineConfig {
  BackendKind backend{BackendKind::Cpu};
  std::string model_path;
  std::optional<int32_t> n_gpu_layers;
  std::optional<int32_t> inference_threads;
  uint32_t max_candidates{9};
  uint32_t max_context_length{10};
  bool enable_live_conversion{true};
  double learning_alpha{0.8};
  size_t learning_flush_every_n{8};
  uint64_t learning_flush_interval_sec{5};
  size_t learning_max_records{10000};
  double learning_min_weight{0.05};
  // Internal prediction policy from user-learning-enhancement-spec.md section 14.6.
  // These values are runtime configuration, not persisted settings keys.
  size_t prediction_learning_max_entries{3};
  double prediction_learning_min_score{0.05};
  // Default score for user-dictionary entries that lack an explicit value.
  double user_word_default_score{1.5};
};

struct ModelLoadOptions {
  std::string path;
  BackendKind backend{BackendKind::Cpu};
  std::optional<int32_t> n_gpu_layers;
  // Optional per-load override. Production callers pass the configured runtime value.
  std::optional<int32_t> n_threads;
  // Test-only fixture switch for no-llama builds; production callers leave this false.
  bool mock_zenzai_candidates_for_tests{false};
  // Test-only hook used to prove loading work runs outside state_mutex_.
  std::function<void()> before_probe_for_tests;
  // Test-only fault injection at each backend load attempt.
  std::function<void(BackendKind)> before_load_for_tests;
};

struct ModelLoadResult {
  bool ok{false};
  std::optional<std::string> error;
};

struct EngineHealthSnapshot {
  BackendKind backend{BackendKind::Cpu};
  bool model_loaded{false};
  bool model_preload_in_progress{false};
  std::string model_path;
  std::optional<std::string> last_error;
  size_t learning_entries{};
  size_t user_dict_entries{};
};

class InferenceEngine {
 public:
  InferenceEngine(std::unique_ptr<core::IConverter> converter, learning::LearningStore* store,
                  EngineConfig config);
  ~InferenceEngine();

  // External, non-owning. May be nullptr (no user dictionary).
  void SetUserDictionary(learning::UserDictionary* dict);
  struct DictionaryLoadResult {
    std::string name;
    bool loaded;
    std::string error;
  };
  std::vector<DictionaryLoadResult> LoadBundledDictionaryLayers(
      const std::filesystem::path& directory);
  bool LoadDictionaryLayer(learning::LayerId layer, const std::filesystem::path& path,
                           bool verify = false);
  void EnableDictionaryLayer(learning::LayerId layer, bool enabled);
  bool AddUserWord(const learning::UserWord& word);
  bool RemoveUserWord(const std::string& word, const std::string& ruby);

  bool LoadModel();
  bool LoadModel(const ModelLoadOptions& options);
  ModelLoadResult LoadModelWithResult();
  ModelLoadResult LoadModelWithResult(const ModelLoadOptions& options);
  bool StartModelPreload(ModelLoadOptions options);
  void WaitForModelPreload();

  // QueryCandidates with optional cancel polling. Returns an empty vector
  // immediately when *cancel is observed true. cancel may be nullptr.
  std::vector<core::Candidate> QueryCandidates(const std::string& kana, const std::string& context,
                                               uint64_t now_epoch_sec,
                                               const std::atomic<bool>* cancel,
                                               uint32_t max_candidates = 0, bool live = false);

  // Backwards-compatible overload without cancel support.
  std::vector<core::Candidate> QueryCandidates(const std::string& kana, const std::string& context,
                                               uint64_t now_epoch_sec);

  std::vector<core::Candidate> QueryPredictions(const std::string& kana, const std::string& context,
                                                uint64_t now_epoch_sec);
  std::vector<core::Candidate> QueryCorrections(const std::string& kana, const std::string& context,
                                                const std::string& rejected_surface,
                                                uint64_t now_epoch_sec);
  // observation_id is the TIP-side idempotency key (DEV-554). A non-empty id
  // that was already applied is ignored so a resend after a pipe drop does not
  // count the same commit twice; an empty id disables dedupe (legacy TIP).
  // Returns false when the observation was dropped as a duplicate.
  bool CommitObservation(const std::string& reading, const std::string& surface,
                         uint64_t now_epoch_sec, const std::string& observation_id = {});
  void CommitCorrection(const std::string& reading, const std::string& rejected_surface,
                        const std::string& selected_surface, uint64_t now_epoch_sec);
  bool FlushLearningStore();
  void ApplyConfig(const EngineConfig& config);

  BackendKind backend() const;
  EngineConfig config() const;
  EngineHealthSnapshot health_snapshot() const;
  bool model_loaded() const;
  bool model_preload_in_progress() const;
  std::optional<std::string> last_error() const;
  std::optional<std::string> effective_last_error() const;
  std::optional<ZenzaiDecodeStats> last_zenzai_decode_stats() const;

 private:
  void NoteLearningMutationLocked(uint64_t now_epoch_sec);
  bool NoteObservationIdLocked(const std::string& observation_id);
  std::vector<core::Candidate> ApplyRerankerOrRaw(const std::string& kana,
                                                  std::vector<core::Candidate> candidates,
                                                  uint64_t now_epoch_sec);
  void MirrorModelRuntimeErrorLocked(const std::shared_ptr<core::IConverter>& converter);
  void RestoreUserDictionaryLocked(const std::vector<learning::UserWord>& entries);
  bool ShouldFlushLearningStoreLocked(uint64_t now_epoch_sec) const;
  bool FlushLearningStoreLocked();
  void RecordLearningSaveFailureLocked();
  void RecordUserDictionaryFailureLocked(const char* error);
  void LearningFlushWorker();
  void RefreshDictionaryLocked();

  std::shared_ptr<core::IConverter> fallback_converter_;
  std::shared_ptr<core::IConverter> model_converter_;
  std::shared_ptr<core::IConverter> active_converter_;
  learning::LearningStore* store_;
  learning::Reranker reranker_;
  learning::UserDictionary* user_dict_{nullptr};
  learning::DictionaryStore dictionaries_;
  const learning::UserDictionary* indexed_user_dict_{nullptr};
  uint64_t indexed_user_revision_{};
  bool user_dictionary_enabled_{true};
  mutable std::mutex state_mutex_;
  mutable std::mutex converter_call_mutex_;
  std::mutex model_load_mutex_;
  std::mutex model_preload_thread_mutex_;
  EngineConfig config_;
  bool model_loaded_{false};
  bool model_preload_in_progress_{false};
  std::optional<std::string> last_error_;
  std::optional<std::string> model_runtime_error_;
  size_t unsaved_observations_{0};
  std::optional<uint64_t> first_unsaved_observation_epoch_sec_;
  std::optional<uint64_t> last_unsaved_observation_epoch_sec_;
  std::optional<std::chrono::steady_clock::time_point> first_unsaved_observation_steady_;
  std::optional<std::chrono::steady_clock::time_point> last_successful_learning_save_steady_;
  // Bounded ring of recently applied observation ids (DEV-554). Depth exceeds
  // the TIP-side resend backlog cap so every item the TIP can still retry is
  // covered. Not persisted: a Host that crashes after Save() but before the ACK
  // sees the resend as new, which double-counts one commit's weight
  // (see docs/learning-data-management-spec.md, at-least-once section).
  std::deque<std::string> applied_observation_ids_;
  std::unordered_set<std::string> applied_observation_id_set_;
  std::condition_variable learning_flush_cv_;
  std::thread learning_flush_thread_;
  std::thread model_preload_thread_;
  bool learning_flush_stop_{false};
};

}  // namespace azookey::host
