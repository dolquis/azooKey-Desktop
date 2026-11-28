#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "azookey/core/Candidate.h"

namespace azookey::reranker {

inline constexpr int kTinyTimeoutMinMs = 10;
inline constexpr int kTinyTimeoutMaxMs = 20;
inline constexpr int kTinyFailureMin = 1;
inline constexpr int kTinyFailureMax = 10;

struct TinyRerankerConfig {
  bool enabled{};
  std::string model_path;
  int timeout_ms{15};
  int failure_threshold{3};
  bool operator==(const TinyRerankerConfig&) const = default;
};

TinyRerankerConfig ClampTinyRerankerConfig(TinyRerankerConfig config);

std::optional<std::filesystem::path> ResolveTinyRerankerModelPath(
    const std::string& explicit_path, const std::filesystem::path& user_models,
    const std::filesystem::path& bundled_models);

class ITinyRerankerRuntime {
 public:
  virtual ~ITinyRerankerRuntime() = default;
  // One [N,12] batch, one score for each row of [N,1]. The watchdog sets stop on timeout
  // or cancellation. Inputs and the runtime remain alive for the duration of Infer.
  virtual std::vector<float> Infer(std::span<const std::array<float, 12>> features,
                                   const std::atomic<bool>& stop) = 0;
};

enum class TinyRerankOutcome {
  Applied,
  Disabled,
  SecureMode,
  LoadFailed,
  InvalidInput,
  Timeout,
  InferError,
  CircuitOpen,
  Skipped,
};

const char* TinyRerankReason(TinyRerankOutcome outcome);

// The caller serializes configuration and inference. At most one forward remains in flight,
// including across session resets. A late forward owns only its runtime, batch, and stop flag.
class TinyRerankerRuntime {
 public:
  using Factory = std::function<std::shared_ptr<ITinyRerankerRuntime>(const std::filesystem::path&)>;
  using Deadline = std::optional<std::chrono::steady_clock::time_point>;

  void SetConfig(TinyRerankerConfig config);
  void SetModelDirectories(std::filesystem::path user_models, std::filesystem::path bundled_models);
  void SetFactoryForTests(Factory factory, std::optional<std::chrono::milliseconds> timeout = {});
  TinyRerankOutcome Apply(std::vector<core::Candidate>& candidates, bool secure,
                          const std::atomic<bool>* cancel = nullptr, Deadline deadline = {});

 private:
  void ResetSession();
  TinyRerankOutcome RecordFailure(TinyRerankOutcome outcome);

  TinyRerankerConfig config_;
  std::filesystem::path user_models_;
  std::filesystem::path bundled_models_;
  Factory factory_;
  std::optional<std::chrono::milliseconds> timeout_for_tests_;
  std::shared_ptr<ITinyRerankerRuntime> runtime_;
  std::future<std::vector<float>> pending_;
  bool load_attempted_{};
  int failures_{};
};

}  // namespace azookey::reranker
