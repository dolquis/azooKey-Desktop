#include "azookey/core/CrashReporting.h"

#include <atomic>
#include <cstddef>
#include <ctime>
#include <cwchar>

#include "azookey/core/CrashRetention.h"
#include "azookey/core/PlatformPaths.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Windows SDK extension headers require the Windows base types first.
// clang-format off
#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <ShlObj.h>
// clang-format on
#include <process.h>
#endif

namespace azookey::core {
namespace {
std::atomic<CrashStatus> status{CrashStatus::Disabled};
#ifdef _WIN32
SRWLOCK state_lock = SRWLOCK_INIT;
std::atomic<CrashConsent> consent{CrashConsent::Off};
CrashModule module = CrashModule::Host;
wchar_t directory_path[32768]{};
std::uint64_t image_base{};
std::uint32_t image_size{};
bool installed{};
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> previous_filter{};
HANDLE worker_thread{};
HANDLE worker_wake{};
HANDLE worker_done{};
std::atomic<bool> worker_stop{};
std::atomic<unsigned> active_filters{};
// One crash request per process lifetime. Concurrent faults fall back to the OS.
enum class RequestState { Idle, Pending, Writing, Done, AbandonedPending, AbandonedWriting };
std::atomic<RequestState> request_state{RequestState::Idle};
std::atomic<bool> request_written{};
std::uint32_t request_code{}, request_thread{};
std::uint64_t request_address{};

struct ExclusiveLock {
  bool held;
  explicit ExclusiveLock(bool try_only = false)
      : held(try_only ? TryAcquireSRWLockExclusive(&state_lock) != FALSE : true) {
    if (!try_only) AcquireSRWLockExclusive(&state_lock);
  }
  ~ExclusiveLock() {
    if (held) ReleaseSRWLockExclusive(&state_lock);
  }
};

// Host and Settings share a retention budget, including concurrent crashes.
struct RetentionLock {
  HANDLE handle{};
  bool held{};
  RetentionLock() {
    std::uint64_t hash = 14695981039346656037ull;
    for (const wchar_t* p = directory_path; *p; ++p) {
      const wchar_t character = *p >= L'A' && *p <= L'Z' ? *p + (L'a' - L'A') : *p;
      hash = (hash ^ static_cast<std::uint64_t>(character)) * 1099511628211ull;
    }
    wchar_t name[80]{};
    std::swprintf(name, std::size(name), L"Local\\azooKey-crashes-%016llx", hash);
    handle = CreateMutexW(nullptr, FALSE, name);
    if (handle) {
      const auto waited = WaitForSingleObject(handle, 100);
      held = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    }
  }
  ~RetentionLock() {
    if (held) ReleaseMutex(handle);
    if (handle) CloseHandle(handle);
  }
};

bool PrepareDirectory() {
  if (!directory_path[0]) return false;
  const int result = SHCreateDirectoryExW(nullptr, directory_path, nullptr);
  if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS && result != ERROR_FILE_EXISTS)
    return false;
  const auto attributes = GetFileAttributesW(directory_path);
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
         !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

// The entire record is explicitly zeroed. None of these streams reference memory,
// a CONTEXT, exception parameters, a TEB, a user path, or dynamically read strings.
#pragma pack(push, 4)
struct OneThreadList {
  ULONG32 NumberOfThreads;
  MINIDUMP_THREAD Threads[1];
};
struct OneModuleList {
  ULONG32 NumberOfModules;
  MINIDUMP_MODULE Modules[1];
};
#pragma pack(pop)
static_assert(offsetof(OneThreadList, Threads) == 4);
static_assert(sizeof(OneThreadList) == 4 + sizeof(MINIDUMP_THREAD));
static_assert(offsetof(OneModuleList, Modules) == 4);
static_assert(sizeof(OneModuleList) == 4 + sizeof(MINIDUMP_MODULE));

struct MetadataDump {
  MINIDUMP_HEADER header;
  MINIDUMP_DIRECTORY streams[4];
  MINIDUMP_SYSTEM_INFO system;
  MINIDUMP_EXCEPTION_STREAM exception;
  OneThreadList threads;
  OneModuleList modules;
  ULONG name_bytes;
  wchar_t name[32];
};

unsigned __stdcall ReportWorker(void*) {
  while (WaitForSingleObject(worker_wake, INFINITE) == WAIT_OBJECT_0 && !worker_stop.load()) {
    auto expected = RequestState::Pending;
    if (!request_state.compare_exchange_strong(expected, RequestState::Writing)) {
      continue;
    }
    request_written.store(
        CrashReporting::WriteReport(request_code, request_address, request_thread));
    const auto previous = request_state.exchange(RequestState::Done);
    if (previous != RequestState::AbandonedWriting) SetEvent(worker_done);
    // Acknowledge the write before retention: slow cleanup must not cause WER
    // fallback after an artifact has already been successfully saved.
    if (request_written.load()) {
      ExclusiveLock lock;
      RetentionLock retention_lock;
      if (retention_lock.held && !worker_stop.load() && consent == CrashConsent::Local &&
          PruneCrashDumps(directory_path).failed)
        status.store(CrashStatus::WriteFailed);
    }
  }
  return 0;
}

bool StartWorker() {
  if (worker_thread) return !worker_stop.load();
  worker_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  worker_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (worker_wake && worker_done) {
    worker_stop.store(false);
    worker_thread =
        reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &ReportWorker, nullptr, 0, nullptr));
    if (worker_thread) return true;
  }
  if (worker_wake) CloseHandle(worker_wake);
  if (worker_done) CloseHandle(worker_done);
  worker_wake = worker_done = nullptr;
  return false;
}

bool RequestReport(EXCEPTION_POINTERS* exception) {
  // Avoid heap/filesystem work and blocking lock acquisition on the faulting thread.
  if (!TryAcquireSRWLockShared(&state_lock)) return false;
  const bool allowed = consent == CrashConsent::Local && worker_thread && !worker_stop.load();
  if (!allowed) {
    ReleaseSRWLockShared(&state_lock);
    return false;
  }
  auto expected = RequestState::Idle;
  if (!request_state.compare_exchange_strong(expected, RequestState::Pending)) {
    ReleaseSRWLockShared(&state_lock);
    return false;
  }
  request_code = exception->ExceptionRecord->ExceptionCode;
  request_address = reinterpret_cast<std::uint64_t>(exception->ExceptionRecord->ExceptionAddress);
  request_thread = GetCurrentThreadId();
  request_written.store(false);
  active_filters.fetch_add(1);
  ResetEvent(worker_done);
  SetEvent(worker_wake);
  ReleaseSRWLockShared(&state_lock);
  WaitForSingleObject(worker_done, 1000);
  for (;;) {
    auto state = request_state.load();
    if (state == RequestState::Done) {
      const bool wrote = request_written.load();
      active_filters.fetch_sub(1);
      return wrote;
    }
    if (state != RequestState::Pending && state != RequestState::Writing) {
      active_filters.fetch_sub(1);
      return false;
    }
    const auto abandoned = state == RequestState::Pending ? RequestState::AbandonedPending
                                                          : RequestState::AbandonedWriting;
    if (request_state.compare_exchange_weak(state, abandoned)) {
      active_filters.fetch_sub(1);
      return false;
    }
  }
}

LONG WINAPI Filter(EXCEPTION_POINTERS* exception) {
  if (exception && exception->ExceptionRecord && RequestReport(exception))
    return EXCEPTION_EXECUTE_HANDLER;
  const auto previous = previous_filter.load(std::memory_order_acquire);
  return previous && previous != &Filter ? previous(exception) : EXCEPTION_CONTINUE_SEARCH;
}
#endif
}  // namespace

std::filesystem::path CrashReporting::DefaultDirectory() noexcept {
  try {
    const auto local = GetLocalAppDataDirectory();
    return local ? *local / "azooKey" / "crashes" : std::filesystem::path{};
  } catch (...) {
    return {};
  }
}

void CrashReporting::Initialize(CrashModule owner, CrashConsent mode,
                                const std::filesystem::path& directory) noexcept {
#ifdef _WIN32
  try {
    {
      ExclusiveLock lock;
      consent = CrashConsent::Off;
      module = owner;
      const auto resolved = directory.empty() ? DefaultDirectory() : directory;
      directory_path[0] = L'\0';
      const auto path = resolved.wstring();
      if (!path.empty() && path.size() < std::size(directory_path) - 80)
        wcscpy_s(directory_path, std::size(directory_path), path.c_str());
      MODULEINFO image{};
      if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &image,
                               sizeof(image))) {
        image_base = reinterpret_cast<std::uint64_t>(image.lpBaseOfDll);
        image_size = image.SizeOfImage;
      }
      if (!installed) {
        previous_filter.store(SetUnhandledExceptionFilter(&Filter), std::memory_order_release);
        installed = true;
      }
    }
    SetConsent(mode);
  } catch (...) {
    status.store(CrashStatus::DirectoryUnavailable, std::memory_order_release);
  }
#else
  (void)owner;
  (void)mode;
  (void)directory;
  status.store(CrashStatus::Unsupported);
#endif
}

void CrashReporting::SetConsent(CrashConsent mode) noexcept {
#ifdef _WIN32
  if (mode == CrashConsent::Off) {
    consent.store(CrashConsent::Off);
    status.store(CrashStatus::Disabled);
    return;
  }
  try {
    ExclusiveLock lock;
    consent = mode;
    if (mode != CrashConsent::Local) {
      status.store(CrashStatus::Disabled, std::memory_order_release);
      return;
    }
    if (!PrepareDirectory() || !StartWorker()) {
      status.store(CrashStatus::DirectoryUnavailable, std::memory_order_release);
      return;
    }
    RetentionLock retention_lock;
    const bool failed = !retention_lock.held || PruneCrashDumps(directory_path).failed;
    status.store(failed ? CrashStatus::WriteFailed : CrashStatus::Ready, std::memory_order_release);
  } catch (...) {
    status.store(CrashStatus::DirectoryUnavailable, std::memory_order_release);
  }
#else
  (void)mode;
#endif
}

void CrashReporting::Shutdown() noexcept {
#ifdef _WIN32
  consent.store(CrashConsent::Off);
  {
    ExclusiveLock lock(true);
    // Process shutdown must not wait on a worker stalled in filesystem code.
    // Keep live handles valid for an outstanding filter; process exit reclaims them.
    if (!lock.held || active_filters.load() != 0) {
      status.store(CrashStatus::Disabled);
      return;
    }
    if (installed) {
      const auto current =
          SetUnhandledExceptionFilter(previous_filter.load(std::memory_order_acquire));
      // Do not remove a filter installed later by another process component.
      if (current != &Filter) SetUnhandledExceptionFilter(current);
      installed = false;
    }
    directory_path[0] = L'\0';
    worker_stop.store(true);
    if (worker_wake) SetEvent(worker_wake);
    if (worker_thread && WaitForSingleObject(worker_thread, 1000) == WAIT_OBJECT_0) {
      CloseHandle(worker_thread);
      CloseHandle(worker_wake);
      CloseHandle(worker_done);
      worker_thread = worker_wake = worker_done = nullptr;
      request_state.store(RequestState::Idle);
    }
  }
#endif
  status.store(CrashStatus::Disabled, std::memory_order_release);
}

CrashStatus CrashReporting::Status() noexcept { return status.load(std::memory_order_acquire); }

bool CrashReporting::WriteReport(std::uint32_t code, std::uint64_t address,
                                 std::uint32_t thread_id) noexcept {
#ifdef _WIN32
  try {
    // The prestarted worker can wait for configuration; the faulting thread only
    // waits for the bounded request. This also avoids racing its short shared lock.
    ExclusiveLock lock;
    if (!lock.held || consent != CrashConsent::Local ||
        request_state.load() == RequestState::AbandonedWriting)
      return false;
    if (!PrepareDirectory()) {
      status.store(CrashStatus::DirectoryUnavailable, std::memory_order_release);
      return false;
    }
    RetentionLock retention_lock;
    if (!retention_lock.held) {
      status.store(CrashStatus::WriteFailed);
      return false;
    }
    MetadataDump dump;
    ZeroMemory(&dump, sizeof(dump));
    // Reserve one report before writing: process termination can interrupt the
    // best-effort post-write pruning immediately after the worker signals done.
    CrashRetentionLimits reserved;
    --reserved.count;
    reserved.bytes -= sizeof(dump);
    if (PruneCrashDumps(directory_path, reserved).failed) {
      status.store(CrashStatus::WriteFailed);
      return false;
    }
    dump.header.Signature = MINIDUMP_SIGNATURE;
    dump.header.Version = MINIDUMP_VERSION;
    dump.header.NumberOfStreams = 4;
    dump.header.StreamDirectoryRva = offsetof(MetadataDump, streams);
    dump.header.TimeDateStamp = static_cast<ULONG>(std::time(nullptr));
    dump.streams[0] = {SystemInfoStream, {sizeof(dump.system), offsetof(MetadataDump, system)}};
    dump.streams[1] = {ExceptionStream,
                       {sizeof(dump.exception), offsetof(MetadataDump, exception)}};
    dump.streams[2] = {ThreadListStream, {sizeof(dump.threads), offsetof(MetadataDump, threads)}};
    dump.streams[3] = {ModuleListStream, {sizeof(dump.modules), offsetof(MetadataDump, modules)}};
#if defined(_M_ARM64)
    dump.system.ProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;
#elif defined(_M_X64)
    dump.system.ProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
#else
    dump.system.ProcessorArchitecture = PROCESSOR_ARCHITECTURE_INTEL;
#endif
    dump.system.MajorVersion = 10;
    dump.system.PlatformId = VER_PLATFORM_WIN32_NT;
    dump.exception.ThreadId = thread_id;
    dump.exception.ExceptionRecord.ExceptionCode = code;
    dump.exception.ExceptionRecord.ExceptionAddress = address;
    dump.threads.NumberOfThreads = 1;
    dump.threads.Threads[0].ThreadId = thread_id;
    dump.modules.NumberOfModules = 1;
    dump.modules.Modules[0].BaseOfImage = image_base;
    dump.modules.Modules[0].SizeOfImage = image_size;
    dump.modules.Modules[0].ModuleNameRva = offsetof(MetadataDump, name_bytes);
    wcscpy_s(dump.name, std::size(dump.name),
             module == CrashModule::Host ? L"azookey_inference_host.exe" : L"azookey_settings.exe");
    dump.name_bytes = static_cast<ULONG>(std::wcslen(dump.name) * sizeof(wchar_t));
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    wchar_t filename[32768]{};
    const auto length = std::swprintf(
        filename, std::size(filename), L"%ls\\azookey-%ls-%04u%02u%02uT%02u%02u%02uZ-%lu.dmp",
        directory_path, module == CrashModule::Host ? L"host" : L"settings", utc.wYear, utc.wMonth,
        utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, GetCurrentProcessId());
    if (length < 0) return false;
    if (consent != CrashConsent::Local || request_state.load() == RequestState::AbandonedWriting)
      return false;
    HANDLE file = CreateFileW(filename, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      status.store(CrashStatus::WriteFailed, std::memory_order_release);
      return false;
    }
    DWORD written{};
    const bool succeeded = WriteFile(file, &dump, sizeof(dump), &written, nullptr) &&
                           written == sizeof(dump) && FlushFileBuffers(file) &&
                           consent == CrashConsent::Local &&
                           request_state.load() != RequestState::AbandonedWriting;
    CloseHandle(file);
    if (!succeeded) DeleteFileW(filename);
    status.store(consent != CrashConsent::Local ? CrashStatus::Disabled
                 : succeeded                    ? CrashStatus::Ready
                                                : CrashStatus::WriteFailed,
                 std::memory_order_release);
    return succeeded;
  } catch (...) {
    status.store(CrashStatus::WriteFailed, std::memory_order_release);
    return false;
  }
#else
  (void)code;
  (void)address;
  (void)thread_id;
  return false;
#endif
}
}  // namespace azookey::core
