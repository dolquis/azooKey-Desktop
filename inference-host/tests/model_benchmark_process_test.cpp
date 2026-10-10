#include <gtest/gtest.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

#include "azookey/core/PlatformPaths.h"
#include "azookey/host/ModelBenchmark.h"
#include "azookey/host/ModelScanner.h"

namespace {

namespace fs = std::filesystem;
using azookey::host::ModelBenchmarkOptions;
using azookey::host::RunModelBenchmark;
using azookey::ipc::BenchmarkModelRequest;
using azookey::ipc::BenchmarkModelResponse;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Includes process startup and the uncooperative call. The upper bound allows
// the bounded reap plus scheduling slack on shared Windows runners.
constexpr auto kStallBudget = 1500ms;
constexpr auto kReturnBound = kStallBudget + 6s;

class ScopedTempDirectory {
 public:
  ScopedTempDirectory() {
    const auto base = fs::temp_directory_path();
    for (int attempt = 0; attempt < 16; ++attempt) {
      const auto candidate =
          base / ("azookey_benchmark_process_" + std::to_string(GetCurrentProcessId()) + "_" +
                  std::to_string(Clock::now().time_since_epoch().count()) + "_" +
                  std::to_string(next_id_++));
      if (fs::create_directory(candidate)) {
        path_ = candidate;
        return;
      }
    }
    throw std::runtime_error("Could not create a unique benchmark test directory");
  }
  ~ScopedTempDirectory() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  ScopedTempDirectory(const ScopedTempDirectory&) = delete;
  ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;
  fs::path File(const char* name) const { return path_ / name; }

 private:
  fs::path path_;
  static inline std::atomic<uint64_t> next_id_{0};
};

void AppendU32(std::string& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

void AppendU64(std::string& out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

void AppendString(std::string& out, const std::string& value) {
  AppendU64(out, value.size());
  out += value;
}

// The same probe-only GGUF shape used by model_scanner_test.cpp. Query/unload
// tests enable mock candidates and therefore need the no-llama build.
std::string GgufHeader() {
  std::string out = "GGUF";
  AppendU32(out, 3);
  AppendU64(out, 1);
  AppendU64(out, 3);
  AppendString(out, "general.name");
  AppendU32(out, 8);
  AppendString(out, "fixture");
  AppendString(out, "general.architecture");
  AppendU32(out, 8);
  AppendString(out, "gpt2");
  AppendString(out, "general.file_type");
  AppendU32(out, 4);
  AppendU32(out, 15);
  return out;
}

class WorkerObservation {
 public:
  ~WorkerObservation() {
    if (process_) CloseHandle(process_);
  }
  WorkerObservation() = default;
  WorkerObservation(const WorkerObservation&) = delete;
  WorkerObservation& operator=(const WorkerObservation&) = delete;

  void Started(uint32_t pid) {
    pid_ = pid;
    process_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    error_ = process_ ? ERROR_SUCCESS : GetLastError();
  }

  void ExpectRunning() const {
    ASSERT_NE(process_, nullptr) << "OpenProcess failed: " << error_;
    EXPECT_EQ(WaitForSingleObject(process_, 0), WAIT_TIMEOUT);
  }

  void ExpectExited() const {
    ASSERT_NE(pid_, 0u) << "the disposable worker was never started";
    ASSERT_NE(process_, nullptr) << "OpenProcess failed: " << error_;
    // Keep a handle from startup, so PID reuse or a vanished process cannot
    // turn this check into a false pass. Exit must precede benchmark return.
    EXPECT_EQ(WaitForSingleObject(process_, 0), WAIT_OBJECT_0);
    DWORD exit_code = STILL_ACTIVE;
    ASSERT_TRUE(GetExitCodeProcess(process_, &exit_code));
    EXPECT_NE(exit_code, STILL_ACTIVE);
  }

 private:
  uint32_t pid_{0};
  HANDLE process_{nullptr};
  DWORD error_{ERROR_SUCCESS};
};

class ReleaseCallbackOnExit {
 public:
  explicit ReleaseCallbackOnExit(std::promise<void>& release) : release_(release) {}
  ~ReleaseCallbackOnExit() { Release(); }
  void Release() {
    if (!released_) {
      release_.set_value();
      released_ = true;
    }
  }

 private:
  std::promise<void>& release_;
  bool released_{false};
};

class ScopedStandardHandles {
 public:
  ScopedStandardHandles() {
    for (size_t i = 0; i < ids_.size(); ++i) saved_[i] = GetStdHandle(ids_[i]);
  }
  ~ScopedStandardHandles() {
    while (changed_ != 0) {
      --changed_;
      SetStdHandle(ids_[changed_], saved_[changed_]);
    }
  }
  ScopedStandardHandles(const ScopedStandardHandles&) = delete;
  ScopedStandardHandles& operator=(const ScopedStandardHandles&) = delete;

  bool Redirect(HANDLE handle) {
    for (const auto id : ids_) {
      if (!SetStdHandle(id, handle)) return false;
      ++changed_;
    }
    return true;
  }

 private:
  const std::array<DWORD, 3> ids_{STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  std::array<HANDLE, 3> saved_{};
  size_t changed_{0};
};

class ModelBenchmarkProcessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto bytes = GgufHeader();
    std::ofstream out(temp_.File("model.gguf"), std::ios::binary);
    ASSERT_TRUE(out);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    ASSERT_TRUE(out);
    ASSERT_TRUE(azookey::host::InspectGgufFile(temp_.File("model.gguf")).valid);
  }

  BenchmarkModelRequest Request(const char* stage = "にほんご") const {
    BenchmarkModelRequest request;
    request.path = azookey::core::PathToUtf8(temp_.File("model.gguf"));
    request.cases = {stage};
    request.iterations = 4;
    request.warmup = 0;
    return request;
  }

  ModelBenchmarkOptions Options(WorkerObservation& observation) const {
    ModelBenchmarkOptions options;
    options.budget = kStallBudget;
    options.mock_zenzai_candidates_for_tests = true;
    options.worker_started_for_tests = [&](uint32_t pid) { observation.Started(pid); };
    return options;
  }

  BenchmarkModelResponse RunStalled(const char* stage,
                                    std::chrono::milliseconds budget = kStallBudget) const {
    WorkerObservation observation;
    auto options = Options(observation);
    options.budget = budget;
    const auto start = Clock::now();
    const auto response = RunModelBenchmark(Request(stage), options);
    EXPECT_LT(Clock::now() - start, budget + 6s);
    EXPECT_EQ(response.status, "timeout") << response.error.value_or("");
    EXPECT_EQ(response.error, "timeout");
    observation.ExpectExited();
    return response;
  }

  void ExpectSlotAvailable() const {
    const auto slot = azookey::host::TryAcquireBenchmarkSlot();
    EXPECT_TRUE(slot.owns_lock());
  }

#if !AZOOKEY_WITH_LLAMA_CPP
  void ExpectSuccessfulRestart() const {
    WorkerObservation observation;
    auto options = Options(observation);
    options.budget = 5s;
    const auto response = RunModelBenchmark(Request(), options);
    EXPECT_EQ(response.status, "success") << response.error.value_or("");
    EXPECT_FALSE(response.error.has_value());
    EXPECT_EQ(response.iterations_completed, Request().iterations);
    observation.ExpectExited();
  }
#endif

  ScopedTempDirectory temp_;
};

TEST_F(ModelBenchmarkProcessTest, TerminatesAnUncooperativeLoadAndReleasesTheSlot) {
  const auto response = RunStalled("hang-load");
  EXPECT_EQ(response.iterations_completed, 0u);
  ExpectSlotAvailable();
#if !AZOOKEY_WITH_LLAMA_CPP
  ExpectSuccessfulRestart();
#endif
}

#if !AZOOKEY_WITH_LLAMA_CPP
TEST_F(ModelBenchmarkProcessTest, TerminatesAnUncooperativeQueryAndKeepsCompletedSamples) {
  const auto response = RunStalled("hang-query", 5s);
  EXPECT_EQ(response.iterations_completed, 2u);
  EXPECT_GT(response.p50_ms, 0.0);
  EXPECT_LE(response.p50_ms, response.p95_ms);
  EXPECT_LE(response.p95_ms, response.p99_ms);
  EXPECT_GT(response.rss_mb, 0.0);
  ExpectSuccessfulRestart();
}

TEST_F(ModelBenchmarkProcessTest, TerminatesAnUncooperativeUnloadAndKeepsAllSamples) {
  const auto response = RunStalled("hang-unload", 5s);
  EXPECT_EQ(response.iterations_completed, Request().iterations);
  EXPECT_GT(response.p50_ms, 0.0);
  EXPECT_LE(response.p50_ms, response.p95_ms);
  EXPECT_LE(response.p95_ms, response.p99_ms);
  EXPECT_GT(response.rss_mb, 0.0);
  ExpectSuccessfulRestart();
}
#endif

TEST_F(ModelBenchmarkProcessTest, RejectsASecondLaunchUntilTheFirstWorkerHasExited) {
  WorkerObservation observation;
  auto options = Options(observation);
  std::promise<void> started;
  auto started_future = started.get_future();
  std::promise<void> release;
  const auto released = release.get_future().share();
  options.worker_started_for_tests = [&](uint32_t pid) {
    observation.Started(pid);
    started.set_value();
    if (released.wait_for(10s) != std::future_status::ready)
      throw std::runtime_error("benchmark test callback was not released");
  };
  auto running = std::async(std::launch::async,
                            [&] { return RunModelBenchmark(Request("hang-load"), options); });
  // Declared after running: assertion failure releases the callback before
  // destruction of std::future joins the async benchmark.
  ReleaseCallbackOnExit release_on_exit(release);
  ASSERT_EQ(started_future.wait_for(5s), std::future_status::ready);
  observation.ExpectRunning();
  unsigned second_launches = 0;
  ModelBenchmarkOptions second_options;
  second_options.worker_started_for_tests = [&](uint32_t) { ++second_launches; };
  const auto start = Clock::now();
  const auto busy = RunModelBenchmark(Request("hang-load"), second_options);
  EXPECT_LT(Clock::now() - start, 1s);
  EXPECT_EQ(busy.status, "error");
  EXPECT_EQ(busy.error, "busy");
  EXPECT_EQ(second_launches, 0u);
  release_on_exit.Release();
  ASSERT_EQ(running.wait_for(kReturnBound), std::future_status::ready);
  const auto response = running.get();
  EXPECT_EQ(response.status, "timeout");
  EXPECT_EQ(response.error, "timeout");
  observation.ExpectExited();
  ExpectSlotAvailable();
#if !AZOOKEY_WITH_LLAMA_CPP
  ExpectSuccessfulRestart();
#endif
}

TEST_F(ModelBenchmarkProcessTest, RepeatedTimeoutsDoNotLeakWorkerHandles) {
  // Warm up process startup before taking a baseline for lazy runtime state.
  RunStalled("hang-load");
  DWORD before = 0;
  ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &before));
  for (int i = 0; i < 4; ++i) {
    // SCOPED_TRACE lazily opens GoogleTest's Windows watcher-thread handle;
    // keep diagnostics from changing the process-wide handle baseline.
    RunStalled("hang-load");
    DWORD after = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &after));
    // RunStalled has closed the extra OpenProcess observation handle as well.
    EXPECT_EQ(after, before) << "timeout iteration " << i;
    ExpectSlotAvailable();
  }
}

TEST_F(ModelBenchmarkProcessTest, WorkerLaunchFailureDoesNotStrandTheSlot) {
  ModelBenchmarkOptions options;
  options.worker_executable_for_tests = temp_.File("missing-worker.exe");
  ASSERT_TRUE(options.worker_executable_for_tests.is_absolute());
  ASSERT_FALSE(fs::exists(options.worker_executable_for_tests));
  unsigned launches = 0;
  options.worker_started_for_tests = [&](uint32_t) { ++launches; };
  const auto start = Clock::now();
  const auto response = RunModelBenchmark(Request(), options);
  EXPECT_LT(Clock::now() - start, kReturnBound);
  EXPECT_EQ(response.status, "error");
  EXPECT_EQ(response.error, "benchmark_failed");
  EXPECT_EQ(launches, 0u);
  ExpectSlotAvailable();
#if !AZOOKEY_WITH_LLAMA_CPP
  ExpectSuccessfulRestart();
#endif
}

TEST_F(ModelBenchmarkProcessTest, ThrowingStartupObserverTerminatesTheWorkerAndReleasesTheSlot) {
  WorkerObservation observation;
  auto options = Options(observation);
  options.worker_started_for_tests = [&](uint32_t pid) {
    observation.Started(pid);
    throw std::runtime_error("test observer failure");
  };
  const auto start = Clock::now();
  const auto response = RunModelBenchmark(Request("hang-load"), options);
  EXPECT_LT(Clock::now() - start, kReturnBound);
  EXPECT_EQ(response.status, "error");
  EXPECT_EQ(response.error, "benchmark_failed");
  observation.ExpectExited();
  ExpectSlotAvailable();
#if !AZOOKEY_WITH_LLAMA_CPP
  ExpectSuccessfulRestart();
#endif
}

#if !AZOOKEY_WITH_LLAMA_CPP
TEST_F(ModelBenchmarkProcessTest, CompletedWorkerAtZeroRemainingWaitKeepsTheResult) {
  WorkerObservation observation;
  auto options = Options(observation);
  auto now = Clock::now();
  options.now_for_tests = [&] { return now; };
  options.worker_started_for_tests = [&](uint32_t pid) {
    observation.Started(pid);
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) throw std::runtime_error("OpenProcess failed");
    const auto waited = WaitForSingleObject(process, 5000);
    CloseHandle(process);
    EXPECT_EQ(waited, WAIT_OBJECT_0);
    now += options.budget;
  };
  const auto response = RunModelBenchmark(Request(), options);
  EXPECT_EQ(response.status, "success");
  EXPECT_FALSE(response.error.has_value());
  EXPECT_EQ(response.iterations_completed, Request().iterations);
  observation.ExpectExited();
  ExpectSlotAvailable();
}

TEST_F(ModelBenchmarkProcessTest, CompletedWaitIsNotOverriddenByALaterDeadlineReading) {
  WorkerObservation observation;
  auto options = Options(observation);
  const auto start = Clock::now();
  unsigned clock_reads = 0;
  options.now_for_tests = [&] {
    // The old implementation reads the clock a third time after a successful
    // wait. Model scheduling delay there without delaying the actual worker.
    return ++clock_reads >= 3 ? start + options.budget : start;
  };
  options.worker_started_for_tests = [&](uint32_t pid) {
    observation.Started(pid);
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) throw std::runtime_error("OpenProcess failed");
    const auto waited = WaitForSingleObject(process, 5000);
    CloseHandle(process);
    if (waited != WAIT_OBJECT_0) throw std::runtime_error("worker did not complete");
  };
  const auto response = RunModelBenchmark(Request(), options);
  EXPECT_EQ(response.status, "success");
  EXPECT_FALSE(response.error.has_value());
  EXPECT_EQ(response.iterations_completed, Request().iterations);
  observation.ExpectExited();
  ExpectSlotAvailable();
}

TEST_F(ModelBenchmarkProcessTest, ReportsWorkerRssWithoutSamplingTheParent) {
  WorkerObservation observation;
  auto options = Options(observation);
  bool sampled_parent = false;
  options.rss_mb = [&] {
    sampled_parent = true;
    return 1'000'000.0;
  };
  const auto response = RunModelBenchmark(Request(), options);
  EXPECT_EQ(response.status, "success");
  EXPECT_FALSE(sampled_parent);
  EXPECT_GT(response.rss_mb, 0.0);
  EXPECT_NE(response.rss_mb, 1'000'000.0);
  observation.ExpectExited();
  ExpectSlotAvailable();
}

TEST_F(ModelBenchmarkProcessTest, PreservesOptionalLoaderSettingsAcrossTheWorkerBoundary) {
  WorkerObservation unset;
  auto options = Options(unset);
  options.budget = 5s;
  EXPECT_EQ(RunModelBenchmark(Request("config-unset"), options).status, "success");
  unset.ExpectExited();

  WorkerObservation explicit_values;
  options = Options(explicit_values);
  options.budget = 5s;
  options.base_config.n_gpu_layers = -1;
  options.base_config.inference_threads = 2;
  EXPECT_EQ(RunModelBenchmark(Request("config-explicit"), options).status, "success");
  explicit_values.ExpectExited();

  WorkerObservation zero;
  options = Options(zero);
  options.budget = 5s;
  options.base_config.n_gpu_layers = 0;
  options.base_config.inference_threads = 0;
  EXPECT_EQ(RunModelBenchmark(Request("config-zero"), options).status, "success");
  zero.ExpectExited();
}
#endif

TEST_F(ModelBenchmarkProcessTest, AnIncompleteWorkerAtTheDeadlineStillTimesOut) {
  WorkerObservation observation;
  auto options = Options(observation);
  auto now = Clock::now();
  options.now_for_tests = [&] { return now; };
  options.worker_started_for_tests = [&](uint32_t pid) {
    observation.Started(pid);
    now += options.budget;
  };
  const auto response = RunModelBenchmark(Request("hang-load"), options);
  EXPECT_EQ(response.status, "timeout");
  EXPECT_EQ(response.error, "timeout");
  EXPECT_EQ(response.iterations_completed, 0u);
  observation.ExpectExited();
  ExpectSlotAvailable();
}

TEST_F(ModelBenchmarkProcessTest, WorkerGetsWritableNullStdioWhenParentHandlesAreRedirected) {
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
  const auto handle =
      CreateFileW(temp_.File("parent-stdio.bin").c_str(), GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS, 0, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  const auto close = [](void* file) { CloseHandle(file); };
  const std::unique_ptr<void, decltype(close)> file(handle, close);
  WorkerObservation observation;
  auto options = Options(observation);
  options.budget = 5s;
  auto request = Request("stdio-handles");
  request.path = azookey::core::PathToUtf8(temp_.File("missing.gguf"));
  BenchmarkModelResponse response;
  {
    ScopedStandardHandles redirected;
    ASSERT_TRUE(redirected.Redirect(file.get()));
    response = RunModelBenchmark(request, options);
  }
  // The worker hook verifies all Win32 and CRT std handles before model
  // validation. A hook failure would instead produce benchmark_failed.
  EXPECT_EQ(response.status, "error");
  EXPECT_EQ(response.error, "invalid_model");
  LARGE_INTEGER bytes{};
  ASSERT_TRUE(GetFileSizeEx(file.get(), &bytes));
  EXPECT_EQ(bytes.QuadPart, 0);
  observation.ExpectExited();
  ExpectSlotAvailable();
}

TEST_F(ModelBenchmarkProcessTest, ACrashOrMissingFinalResultCannotReportSuccess) {
  for (const auto* stage : {"crash-worker", "no-result-worker"}) {
    SCOPED_TRACE(stage);
    WorkerObservation observation;
    auto options = Options(observation);
    options.budget = 5s;
    const auto response = RunModelBenchmark(Request(stage), options);
    EXPECT_EQ(response.status, "error");
    EXPECT_EQ(response.error, "benchmark_failed");
    observation.ExpectExited();
    ExpectSlotAvailable();
  }
}

TEST_F(ModelBenchmarkProcessTest, ProductionHostWorkerUsesTheInternalEntryPoint) {
  WorkerObservation observation;
  auto options = Options(observation);
  options.budget = 5s;
  options.worker_executable_for_tests = fs::path(AZOOKEY_BENCHMARK_HOST_EXECUTABLE);
  auto request = Request();
  request.path = azookey::core::PathToUtf8(temp_.File("missing.gguf"));
  const auto response = RunModelBenchmark(request, options);
  EXPECT_EQ(response.status, "error");
  EXPECT_EQ(response.error, "invalid_model");
  observation.ExpectExited();
  ExpectSlotAvailable();
}

}  // namespace
#endif
