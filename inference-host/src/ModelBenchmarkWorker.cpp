#include "azookey/host/ModelBenchmarkWorker.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace azookey::host {

#ifdef _WIN32
namespace {

// Input is immutable after launch. Only the child writes progress, alternating
// buffers and committing the index atomically. The parent reads it only after
// process exit, so a killed writer cannot expose a half-written snapshot.
constexpr size_t kProgressBytes = 2048;
constexpr DWORD kReapBudgetMs = 1000;
struct SharedBenchmark {
  uint32_t request_bytes{0};
  int n_gpu_layers{0};
  int inference_threads{0};
  uint32_t has_n_gpu_layers{0};
  uint32_t has_inference_threads{0};
  uint32_t mock_candidates{0};
  LONG progress_index{-1};
  char progress[2][kProgressBytes]{};
};

class WorkerHandle {
 public:
  explicit WorkerHandle(HANDLE handle = nullptr) : handle_(handle) {}
  ~WorkerHandle() {
    if (handle_) CloseHandle(handle_);
  }
  WorkerHandle(const WorkerHandle&) = delete;
  WorkerHandle& operator=(const WorkerHandle&) = delete;
  HANDLE get() const { return handle_; }
  void reset(HANDLE handle) {
    if (handle_) CloseHandle(handle_);
    handle_ = handle;
  }

 private:
  HANDLE handle_;
};

struct BenchmarkChild {
  WorkerHandle process;
  WorkerHandle mapping;
  WorkerHandle job;  // Close before process: KILL_ON_JOB_CLOSE is the backstop.
  SharedBenchmark* shared{nullptr};
  ~BenchmarkChild() {
    if (shared) UnmapViewOfFile(shared);
  }
};

// Protected by TryAcquireBenchmarkSlot. A driver/pending-I/O stall can delay
// process exit even after termination. Retain exactly one such child, with its
// job and process handles, and refuse admission until exit is observed.
std::unique_ptr<BenchmarkChild>& RetiredWorker() {
  static std::unique_ptr<BenchmarkChild> child;
  return child;
}

class WorkerAttributes {
 public:
  bool Initialize(HANDLE mapping, HANDLE job, HANDLE input, HANDLE output) {
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
    storage_.resize(bytes);
    list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    if (!InitializeProcThreadAttributeList(list_, 2, 0, &bytes)) {
      list_ = nullptr;
      return false;
    }
    inherited_handles_ = {mapping, input, output};
    job_ = job;
    return UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                     inherited_handles_.data(), sizeof(inherited_handles_), nullptr,
                                     nullptr) &&
           UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job_, sizeof(job_),
                                     nullptr, nullptr);
  }
  ~WorkerAttributes() {
    if (list_) DeleteProcThreadAttributeList(list_);
  }
  LPPROC_THREAD_ATTRIBUTE_LIST get() const { return list_; }

 private:
  std::vector<unsigned char> storage_;
  LPPROC_THREAD_ATTRIBUTE_LIST list_{nullptr};
  std::array<HANDLE, 3> inherited_handles_{};
  HANDLE job_{nullptr};
};

std::filesystem::path WorkerExecutable(const ModelBenchmarkOptions& options) {
  if (!options.worker_executable_for_tests.empty()) return options.worker_executable_for_tests;
  std::vector<wchar_t> path(32768);
  const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (count == 0 || count >= path.size()) return {};
  return std::filesystem::path(std::wstring(path.data(), count));
}

bool StartWorker(BenchmarkChild& child, const ipc::BenchmarkModelRequest& request,
                 const ModelBenchmarkOptions& options) {
  const auto executable = WorkerExecutable(options);
  if (!executable.is_absolute()) return false;
  const auto input = ipc::BuildBenchmarkModelRequest(request);
  if (input.size() > std::numeric_limits<uint32_t>::max() - sizeof(SharedBenchmark)) return false;
  const auto bytes = static_cast<DWORD>(sizeof(SharedBenchmark) + input.size());
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
  child.mapping.reset(
      CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, bytes, nullptr));
  if (!child.mapping.get()) return false;
  void* view = MapViewOfFile(child.mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, bytes);
  if (!view) return false;
  child.shared = new (view) SharedBenchmark{};
  child.shared->request_bytes = static_cast<uint32_t>(input.size());
  child.shared->n_gpu_layers = options.base_config.n_gpu_layers.value_or(0);
  child.shared->inference_threads = options.base_config.inference_threads.value_or(0);
  child.shared->has_n_gpu_layers = options.base_config.n_gpu_layers.has_value() ? 1 : 0;
  child.shared->has_inference_threads = options.base_config.inference_threads.has_value() ? 1 : 0;
  child.shared->mock_candidates = options.mock_zenzai_candidates_for_tests ? 1 : 0;
  std::memcpy(child.shared + 1, input.data(), input.size());

  child.job.reset(CreateJobObjectW(nullptr, nullptr));
  if (!child.job.get()) return false;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
  limits.BasicLimitInformation.ActiveProcessLimit = 1;
  if (!SetInformationJobObject(child.job.get(), JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits)))
    return false;
  // Give the child real std handles without inheriting the Host's pipes/files.
  const auto input_handle = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &attributes, OPEN_EXISTING, 0, nullptr);
  if (input_handle == INVALID_HANDLE_VALUE) return false;
  WorkerHandle null_input(input_handle);
  const auto output_handle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         &attributes, OPEN_EXISTING, 0, nullptr);
  if (output_handle == INVALID_HANDLE_VALUE) return false;
  WorkerHandle null_output(output_handle);
  WorkerAttributes startup_attributes;
  if (!startup_attributes.Initialize(child.mapping.get(), child.job.get(), null_input.get(),
                                     null_output.get()))
    return false;
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = null_input.get();
  startup.StartupInfo.hStdOutput = null_output.get();
  startup.StartupInfo.hStdError = null_output.get();
  startup.lpAttributeList = startup_attributes.get();
  auto command = L"\"" + executable.wstring() + L"\" --model-benchmark-worker " +
                 std::to_wstring(reinterpret_cast<uintptr_t>(child.mapping.get()));
  PROCESS_INFORMATION process{};
  // Atomic job assignment avoids the orphan window between CreateProcess and
  // AssignProcessToJobObject if the Host is terminated during startup.
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                      &startup.StartupInfo, &process))
    return false;
  child.process.reset(process.hProcess);
  WorkerHandle thread(process.hThread);
  return true;
}

void PublishProgress(SharedBenchmark& shared, const ipc::BenchmarkModelResponse& response) {
  const auto json = ipc::BuildBenchmarkModelResponse(response);
  if (json.size() >= kProgressBytes) return;
  const LONG next = shared.progress_index == 0 ? 1 : 0;
  std::memcpy(shared.progress[next], json.c_str(), json.size() + 1);
  InterlockedExchange(&shared.progress_index, next);
}

std::optional<ipc::BenchmarkModelResponse> ReadProgress(const SharedBenchmark& shared) {
  const auto index = shared.progress_index;
  if (index != 0 && index != 1) return std::nullopt;
  const auto* start = shared.progress[index];
  const auto* end = std::find(start, start + kProgressBytes, '\0');
  if (end == start + kProgressBytes) return std::nullopt;
  const std::string json(start, end);
  const auto response = ipc::ParseBenchmarkModelResponse(json);
  // This private channel always uses the canonical full response builder.
  // The public IPC parser deliberately tolerates missing optional fields;
  // those defaults must not turn a truncated worker snapshot into success.
  if (!response || ipc::BuildBenchmarkModelResponse(*response) != json) return std::nullopt;
  return response;
}

}  // namespace

bool ReapTerminatedBenchmarkWorker() {
  auto& child = RetiredWorker();
  if (!child) return true;
  if (WaitForSingleObject(child->process.get(), 0) != WAIT_OBJECT_0) return false;
  child.reset();
  return true;
}

ipc::BenchmarkModelResponse RunIsolatedModelBenchmark(const ipc::BenchmarkModelRequest& request,
                                                      const ModelBenchmarkOptions& options) {
  ipc::BenchmarkModelResponse response;
  response.backend = request.backend.empty() ? "cpu" : request.backend;
  response.error = "benchmark_failed";
  if (options.budget <= std::chrono::milliseconds::zero()) {
    response.status = "timeout";
    response.error = "timeout";
    return response;
  }
  const auto now = [&] {
    return options.now_for_tests ? options.now_for_tests() : std::chrono::steady_clock::now();
  };
  const auto deadline = now() + options.budget;
  auto child = std::make_unique<BenchmarkChild>();
  if (!StartWorker(*child, request, options)) return response;
  DWORD waited = WAIT_FAILED;
  bool expired = false;
  try {
    if (options.worker_started_for_tests)
      options.worker_started_for_tests(GetProcessId(child->process.get()));
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now());
    waited = WaitForSingleObject(child->process.get(),
                                 static_cast<DWORD>(std::clamp<int64_t>(
                                     remaining.count(), 0, static_cast<int64_t>(INFINITE) - 1)));
    // A signaled process has completed; later scheduling of this thread must
    // not turn a successful wait into a timeout.
    expired = waited != WAIT_OBJECT_0 && now() >= deadline;
  } catch (...) {
    // A test observer must not strand a running process on exception.
    waited = WAIT_FAILED;
  }
  const bool timed_out = waited == WAIT_TIMEOUT || expired;
  if (waited != WAIT_OBJECT_0) {
    // Non-cooperative model / backend calls are confined to this job. Never
    // join indefinitely, detach a thread, or release an untracked GPU worker.
    if (!TerminateJobObject(child->job.get(), 1)) (void)TerminateProcess(child->process.get(), 1);
    if (WaitForSingleObject(child->process.get(), kReapBudgetMs) != WAIT_OBJECT_0) {
      RetiredWorker() = std::move(child);
      if (timed_out) {
        response.status = "timeout";
        response.error = "timeout";
      }
      return response;
    }
  }
  DWORD exit_code = 1;
  const bool exited_ok = GetExitCodeProcess(child->process.get(), &exit_code) && exit_code == 0;
  // On timeout retain only atomically committed progress, even if the final
  // iteration / unload was interrupted. On crash never report false success.
  if (timed_out || (waited == WAIT_OBJECT_0 && exited_ok)) {
    if (const auto progress = ReadProgress(*child->shared)) response = *progress;
  }
  if (timed_out) {
    response.status = "timeout";
    response.error = "timeout";
  }
  return response;
}
#endif

int RunModelBenchmarkWorker(const std::string& mapping_handle,
                            const BenchmarkWorkerTestHooks& hooks) {
#ifdef _WIN32
  uintptr_t value = 0;
  const auto parsed =
      std::from_chars(mapping_handle.data(), mapping_handle.data() + mapping_handle.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != mapping_handle.data() + mapping_handle.size() ||
      value == 0)
    return 2;
  WorkerHandle mapping(reinterpret_cast<HANDLE>(value));
  auto* shared =
      static_cast<SharedBenchmark*>(MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, 0));
  if (!shared) return 2;
  const auto unmap = [](SharedBenchmark* view) { UnmapViewOfFile(view); };
  std::unique_ptr<SharedBenchmark, decltype(unmap)> view(shared, unmap);
  MEMORY_BASIC_INFORMATION memory{};
  if (!VirtualQuery(shared, &memory, sizeof(memory)) ||
      memory.RegionSize < sizeof(SharedBenchmark) ||
      shared->request_bytes > memory.RegionSize - sizeof(SharedBenchmark))
    return 2;
  const auto request = ipc::ParseBenchmarkModelRequest(
      std::string(reinterpret_cast<const char*>(shared + 1), shared->request_bytes));
  if (!request) return 2;
  ModelBenchmarkOptions options;
  options.base_config.n_gpu_layers =
      shared->has_n_gpu_layers ? std::optional<int32_t>{shared->n_gpu_layers} : std::nullopt;
  options.base_config.inference_threads = shared->has_inference_threads
                                              ? std::optional<int32_t>{shared->inference_threads}
                                              : std::nullopt;
  options.mock_zenzai_candidates_for_tests = shared->mock_candidates != 0;
  try {
    if (hooks.options_received) hooks.options_received(*request, options);
    const auto response = RunModelBenchmarkInline(
        *request, options,
        [&](const ipc::BenchmarkModelResponse& progress) { PublishProgress(*shared, progress); },
        hooks);
    PublishProgress(*shared, response);
    return 0;
  } catch (...) {
    return 2;
  }
#else
  (void)mapping_handle;
  (void)hooks;
  return 2;
#endif
}

}  // namespace azookey::host
