// clang-format off
#include <Windows.h>
#include <TlHelp32.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <optional>
#include <string_view>
#include <vector>

#include "cases/HostHangWatchdog.h"
#include "cases/HostProcessSupport.h"

namespace azookey::compat_test::host_hang {
namespace {

constexpr auto kHangLimit = std::chrono::seconds(8);

std::optional<std::uint64_t> ParseUnsigned(const wchar_t* value) {
  if (!value || !*value || *value == L'-') return std::nullopt;
  wchar_t* end = nullptr;
  errno = 0;
  const auto number = std::wcstoull(value, &end, 10);
  if (errno == ERANGE || *end != L'\0') return std::nullopt;
  return number;
}

HANDLE OpenActualParent(DWORD parent_id, std::uint64_t parent_creation_time) {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool valid = false;
  for (BOOL found = Process32FirstW(snapshot, &entry); found;
       found = Process32NextW(snapshot, &entry)) {
    if (entry.th32ProcessID == GetCurrentProcessId()) {
      valid = entry.th32ParentProcessID == parent_id;
      break;
    }
  }
  CloseHandle(snapshot);
  if (!valid) return nullptr;
  const HANDLE parent =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, parent_id);
  std::uint64_t actual_creation_time = 0;
  valid = parent && WaitForSingleObject(parent, 0) == WAIT_TIMEOUT &&
          host_process::ProcessCreationTime(parent, &actual_creation_time) &&
          actual_creation_time == parent_creation_time;
  if (!valid && parent) CloseHandle(parent);
  return valid ? parent : nullptr;
}

bool ResumePending(const std::vector<HANDLE>& threads, SuspensionLedger* ledger) {
  for (int attempt = 0; attempt < 3 && !ledger->AllResumed(); ++attempt) {
    for (size_t i = threads.size(); i > 0; --i) {
      const size_t index = i - 1;
      if (!ledger->NeedsResume(index)) continue;
      if (ResumeThread(threads[index]) != static_cast<DWORD>(-1) ||
          WaitForSingleObject(threads[index], 0) == WAIT_OBJECT_0) {
        ledger->MarkResumed(index);
      }
    }
    if (!ledger->AllResumed()) Sleep(20);
  }
  return ledger->AllResumed();
}

bool AllHostThreadsCovered(DWORD host_id, const std::vector<DWORD>& thread_ids) {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return false;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  bool covered = Thread32First(snapshot, &entry) != FALSE;
  if (covered) {
    do {
      if (entry.th32OwnerProcessID == host_id &&
          std::find(thread_ids.begin(), thread_ids.end(), entry.th32ThreadID) == thread_ids.end()) {
        covered = false;
        break;
      }
    } while (Thread32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return covered;
}

std::optional<host_process::HostProcess> IdentifyHost(DWORD host_id, std::uint64_t creation_time) {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  std::optional<host_process::HostProcess> host;
  for (BOOL found = Process32FirstW(snapshot, &entry); found;
       found = Process32NextW(snapshot, &entry)) {
    if (entry.th32ProcessID == host_id &&
        _wcsicmp(entry.szExeFile, L"azookey_inference_host.exe") == 0) {
      host = host_process::HostProcess{host_id, entry.th32ParentProcessID, creation_time};
      break;
    }
  }
  CloseHandle(snapshot);
  DWORD own_session = 0;
  DWORD host_session = 0;
  if (!host || !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) ||
      !ProcessIdToSessionId(host_id, &host_session) || own_session != host_session) {
    return std::nullopt;
  }
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, host_id);
  const bool valid = process && host_process::IsSameProcess(process, *host);
  if (process) CloseHandle(process);
  return valid ? host : std::nullopt;
}

}  // namespace

int Run(int argc, wchar_t** argv) {
  // Explicit handoff: this process alone owns every SuspendThread/ResumeThread pair.
  if (argc != 9 || std::wstring_view(argv[1]) != L"--internal-c013-watchdog") return 64;
  std::array<std::uint64_t, 7> values{};
  for (size_t i = 0; i < values.size(); ++i) {
    const auto parsed = ParseUnsigned(argv[i + 2]);
    if (!parsed) return 64;
    values[i] = *parsed;
  }
  if (values[0] == 0 || values[0] > MAXDWORD || values[2] == 0 || values[2] > MAXDWORD) {
    return 64;
  }
  const auto host_id = static_cast<DWORD>(values[0]);
  const auto host_created = values[1];
  const auto parent_id = static_cast<DWORD>(values[2]);
  const auto parent_created = values[3];
  const HANDLE ready = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(values[4]));
  const HANDLE release = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(values[5]));
  const HANDLE done = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(values[6]));
  DWORD ignored = 0;
  if (!ready || !release || !done || !GetHandleInformation(ready, &ignored) ||
      !GetHandleInformation(release, &ignored) || !GetHandleInformation(done, &ignored)) {
    return 64;
  }
  const HANDLE parent_process_handle = OpenActualParent(parent_id, parent_created);
  if (!parent_process_handle) return 64;
  const auto host = IdentifyHost(host_id, host_created);
  if (!host) {
    CloseHandle(parent_process_handle);
    return 64;
  }
  const HANDLE host_process_handle =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, host_id);
  if (!host_process_handle || !parent_process_handle ||
      !host_process::IsSameProcess(host_process_handle, *host)) {
    if (host_process_handle) CloseHandle(host_process_handle);
    if (parent_process_handle) CloseHandle(parent_process_handle);
    return 64;
  }
  const auto deadline = std::chrono::steady_clock::now() + kHangLimit;
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    CloseHandle(host_process_handle);
    CloseHandle(parent_process_handle);
    return 64;
  }
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  std::vector<DWORD> thread_ids;
  for (BOOL found = Thread32First(snapshot, &entry); found;
       found = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID == host_id) thread_ids.push_back(entry.th32ThreadID);
  }
  CloseHandle(snapshot);
  if (thread_ids.empty()) {
    CloseHandle(host_process_handle);
    CloseHandle(parent_process_handle);
    return 64;
  }
  std::vector<HANDLE> threads;
  threads.reserve(thread_ids.size());
  SuspensionLedger ledger(thread_ids.size());
  bool suspended = true;
  for (size_t i = 0; i < thread_ids.size(); ++i) {
    HANDLE thread =
        OpenThread(THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                   thread_ids[i]);
    if (!thread || GetProcessIdOfThread(thread) != host_id ||
        !host_process::IsSameProcess(host_process_handle, *host) ||
        WaitForSingleObject(parent_process_handle, 0) != WAIT_TIMEOUT) {
      if (thread) CloseHandle(thread);
      suspended = false;
      break;
    }
    threads.push_back(thread);
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
      suspended = false;
      break;
    }
    ledger.MarkSuspended(i);
  }
  suspended = suspended && host_process::IsSameProcess(host_process_handle, *host) &&
              AllHostThreadsCovered(host_id, thread_ids);
  if (suspended) suspended = SetEvent(ready) != FALSE;
  WakeReason wake = WakeReason::Error;
  if (suspended) {
    const std::array<HANDLE, 2> wait_handles{release, parent_process_handle};
    const auto now = std::chrono::steady_clock::now();
    const auto remaining =
        now >= deadline
            ? 0
            : static_cast<DWORD>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    wake = ClassifyWake(WaitForMultipleObjects(static_cast<DWORD>(wait_handles.size()),
                                               wait_handles.data(), FALSE, remaining));
  }
  const bool resumed = ResumePending(threads, &ledger);
  SetEvent(done);
  for (HANDLE thread : threads) CloseHandle(thread);
  CloseHandle(parent_process_handle);
  CloseHandle(host_process_handle);
  CloseHandle(ready);
  CloseHandle(release);
  CloseHandle(done);
  if (!resumed) return 3;
  if (!suspended) return 2;
  return wake == WakeReason::Release ? 0 : 5;
}

}  // namespace azookey::compat_test::host_hang

int wmain(int argc, wchar_t** argv) { return azookey::compat_test::host_hang::Run(argc, argv); }
