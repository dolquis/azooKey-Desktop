#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "azookey/host/InferenceEngine.h"
#include "azookey/ipc/Payloads.h"

namespace azookey::host {

// docs/model-management-spec.md sections 4.2 and 8.
inline constexpr uint32_t kMaxBenchmarkIterations = 1000;
inline constexpr uint32_t kMaxBenchmarkWarmup = 100;
inline constexpr size_t kMaxBenchmarkCases = 32;
inline constexpr size_t kMaxBenchmarkCaseBytes = 256;
inline constexpr std::chrono::milliseconds kBenchmarkBudget{60'000};

struct ModelBenchmarkOptions {
  // Source of n_gpu_layers / inference_threads; the model path and backend
  // come from the request.
  EngineConfig base_config;
  std::chrono::milliseconds budget{kBenchmarkBudget};
  // Process working set in MiB; null measures this process (0 off Windows).
  std::function<double()> rss_mb;
  // Test-only: lets a no-llama build answer from the probe-only GGUF fixture.
  bool mock_zenzai_candidates_for_tests{false};
  // Test-only process seam. Production relaunches its own executable.
  std::filesystem::path worker_executable_for_tests;
  std::function<void(uint32_t)> worker_started_for_tests;
  std::function<std::chrono::steady_clock::time_point()> now_for_tests;
};

std::vector<std::string> DefaultBenchmarkCases();

// Section 4.2: one benchmark per Host process. The returned lock owns the slot;
// an unlocked result means another benchmark is running ("busy").
std::unique_lock<std::mutex> TryAcquireBenchmarkSlot();

// On Windows, loads the model in a disposable child process (never the live
// engine). The process-wide slot is held until the child has exited; a delayed
// termination is retained as the sole worker and blocks further launches.
// Runs warmup + iterations of QueryCandidates over the cases in turn and reports
// latency percentiles. Exceeding the budget (load included) stops the run with
// status "timeout" and the iterations completed so far. Invalid requests and
// load failures report status "error" with a fixed error category.
ipc::BenchmarkModelResponse RunModelBenchmark(const ipc::BenchmarkModelRequest& request,
                                              const ModelBenchmarkOptions& options);

}  // namespace azookey::host
