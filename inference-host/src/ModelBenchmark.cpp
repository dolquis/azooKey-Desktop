#include "azookey/host/ModelBenchmark.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <system_error>

#include "azookey/core/PlatformPaths.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/ModelBenchmarkWorker.h"
#include "azookey/host/ModelScanner.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Psapi.h requires the Windows SDK types declared by Windows.h.
// clang-format off
#include <Windows.h>
#include <Psapi.h>
// clang-format on
#endif

namespace azookey::host {

namespace {

double ProcessWorkingSetMb() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
#endif
  return 0.0;
}

using Clock = std::chrono::steady_clock;

double ElapsedMs(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

// Nearest-rank percentile over sorted samples.
double Percentile(const std::vector<double>& sorted, double percentile) {
  if (sorted.empty()) return 0.0;
  const auto rank = static_cast<size_t>(std::ceil(percentile / 100.0 * sorted.size()));
  return sorted[std::clamp<size_t>(rank, 1, sorted.size()) - 1];
}

std::optional<BackendKind> BenchmarkBackend(const std::string& backend) {
  if (backend.empty() || backend == "cpu") return BackendKind::Cpu;
  if (backend == "cuda") return BackendKind::Cuda;
  if (backend == "vulkan") return BackendKind::Vulkan;
  return std::nullopt;
}

ipc::BenchmarkModelResponse Error(ipc::BenchmarkModelResponse response, const char* category) {
  response.status = "error";
  response.error = category;
  return response;
}

}  // namespace

std::unique_lock<std::mutex> TryAcquireBenchmarkSlot() {
  static std::mutex slot;
  std::unique_lock<std::mutex> lock(slot, std::try_to_lock);
#ifdef _WIN32
  if (lock.owns_lock() && !ReapTerminatedBenchmarkWorker()) lock.unlock();
#endif
  return lock;
}

std::vector<std::string> DefaultBenchmarkCases() {
  return {"にほんご", "わたし", "こんにちは", "きょうはいいてんき"};
}

ipc::BenchmarkModelResponse RunModelBenchmark(const ipc::BenchmarkModelRequest& request,
                                              const ModelBenchmarkOptions& options) {
  const auto slot = TryAcquireBenchmarkSlot();
  ipc::BenchmarkModelResponse response;
  response.backend = request.backend.empty() ? "cpu" : request.backend;
  if (!slot.owns_lock()) return Error(std::move(response), "busy");
  const auto backend = BenchmarkBackend(request.backend);
  if (!backend) return Error(std::move(response), "unsupported_backend");
  if (request.iterations == 0 || request.iterations > kMaxBenchmarkIterations ||
      request.warmup > kMaxBenchmarkWarmup || request.cases.size() > kMaxBenchmarkCases) {
    return Error(std::move(response), "invalid_request");
  }
  for (const auto& c : request.cases) {
    if (c.empty() || c.size() > kMaxBenchmarkCaseBytes)
      return Error(std::move(response), "invalid_request");
  }
#ifdef _WIN32
  response = RunIsolatedModelBenchmark(request, options);
#else
  response = RunModelBenchmarkInline(request, options);
#endif
  response.rss_mb = options.rss_mb ? options.rss_mb() : ProcessWorkingSetMb();
  return response;
}

ipc::BenchmarkModelResponse RunModelBenchmarkInline(
    const ipc::BenchmarkModelRequest& request, const ModelBenchmarkOptions& options,
    const std::function<void(const ipc::BenchmarkModelResponse&)>& publish,
    const BenchmarkWorkerTestHooks& hooks) {
  ipc::BenchmarkModelResponse response;
  response.backend = request.backend.empty() ? "cpu" : request.backend;
  const auto backend = BenchmarkBackend(request.backend);
  if (!backend) return Error(std::move(response), "unsupported_backend");
  const auto cases = request.cases.empty() ? DefaultBenchmarkCases() : request.cases;
  // File I/O is confined to the worker as well: a stalled probe must not pin
  // the Host's connection thread or admission slot.
  const auto inspected = InspectGgufFile(core::Utf8Path(request.path));
  if (!inspected.valid) return Error(std::move(response), "invalid_model");

  const auto start = Clock::now();
  const auto deadline = start + options.budget;
  EngineConfig config;
  config.n_gpu_layers = options.base_config.n_gpu_layers;
  config.inference_threads = options.base_config.inference_threads;
  config.backend = *backend;
  InferenceEngine engine(std::make_unique<core::SimpleConverter>(), nullptr, config);
  ModelLoadOptions load;
  load.path = request.path;
  load.backend = *backend;
  load.n_gpu_layers = config.n_gpu_layers;
  load.n_threads = config.inference_threads;
  load.mock_zenzai_candidates_for_tests = options.mock_zenzai_candidates_for_tests;
  if (hooks.before_load) hooks.before_load(request);
  const auto loaded = engine.LoadModelWithResult(load);
  response.load_ms = ElapsedMs(start, Clock::now());
  if (!loaded.ok) {
    response = Error(std::move(response), "load_failed");
    if (publish) publish(response);
    return response;
  }
  // The engine may have fallen back (CUDA is not linked yet; Vulkan init can
  // fail): report the backend that actually ran, not the one requested.
  response.backend = BackendName(engine.health_snapshot().backend);
  if (publish) publish(response);

  const auto timed_out = [&] { return Clock::now() >= deadline; };
  std::vector<double> samples;
  samples.reserve(request.iterations);
  bool timeout = false;
  const auto report_samples = [&] {
    response.iterations_completed = static_cast<uint32_t>(samples.size());
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    response.p50_ms = Percentile(sorted, 50.0);
    response.p95_ms = Percentile(sorted, 95.0);
    response.p99_ms = Percentile(sorted, 99.0);
    if (publish) publish(response);
  };
  const uint64_t total = uint64_t{request.warmup} + request.iterations;
  for (uint64_t i = 0; i < total; ++i) {
    if (timed_out()) {
      timeout = true;
      break;
    }
    const auto& reading = cases[static_cast<size_t>(i % cases.size())];
    if (hooks.before_query) hooks.before_query(request, i);
    const auto query_start = Clock::now();
    (void)engine.QueryCandidates(reading, "", 0, nullptr);
    if (i >= request.warmup) samples.push_back(ElapsedMs(query_start, Clock::now()));
    report_samples();
  }

  // No backend here reports device memory yet; null rather than a fake 0.
  response.vram_mb = std::nullopt;
  if (timeout) {
    response.status = "timeout";
    response.error = "timeout";
  } else {
    response.status = "success";
    response.error = std::nullopt;
  }
  if (publish) publish(response);
  if (hooks.before_unload) hooks.before_unload(request);
  return response;
}

}  // namespace azookey::host
