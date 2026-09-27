#include "cases/HostHangWatchdogClient.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace azookey::compat_test::host_hang {
namespace {

std::wstring ExecutablePath() {
  wchar_t path[32768]{};
  const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  if (length == 0 || length >= std::size(path)) return {};
  return (std::filesystem::path(path).parent_path() / L"compat_host_hang_watchdog.exe").wstring();
}

}  // namespace

WatchdogClient::~WatchdogClient() {
  if (child_process_ && WaitForSingleObject(child_process_, 0) == WAIT_TIMEOUT) Resume();
  if (host_process_handle_) CloseHandle(host_process_handle_);
  if (child_process_) CloseHandle(child_process_);
  if (ready_) CloseHandle(ready_);
  if (release_) CloseHandle(release_);
  if (done_) CloseHandle(done_);
}

bool WatchdogClient::Start() {
  host_process_handle_ =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, host_.process_id);
  if (!host_process_handle_ || !host_process::IsSameProcess(host_process_handle_, host_)) {
    return false;
  }
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  ready_ = CreateEventW(&security, TRUE, FALSE, nullptr);
  release_ = CreateEventW(&security, TRUE, FALSE, nullptr);
  done_ = CreateEventW(&security, TRUE, FALSE, nullptr);
  if (!ready_ || !release_ || !done_) return false;

  std::uint64_t parent_created = 0;
  if (!host_process::ProcessCreationTime(GetCurrentProcess(), &parent_created)) return false;
  const auto executable = ExecutablePath();
  if (executable.empty()) return false;
  std::array<HANDLE, 3> handles{ready_, release_, done_};
  SIZE_T attribute_bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
  if (!attribute_bytes) return false;
  std::vector<std::byte> attribute_storage(attribute_bytes);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes)) return false;
  const bool updated =
      UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                sizeof(handles), nullptr, nullptr) != FALSE;
  if (!updated) {
    DeleteProcThreadAttributeList(attributes);
    return false;
  }
  std::wstring command =
      L"\"" + executable + L"\" --internal-c013-watchdog " + std::to_wstring(host_.process_id) +
      L" " + std::to_wstring(host_.creation_time) + L" " + std::to_wstring(GetCurrentProcessId()) +
      L" " + std::to_wstring(parent_created);
  for (HANDLE handle : handles) {
    command += L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(handle));
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION child{};
  const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                      EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
                                      nullptr, &startup.StartupInfo, &child);
  DeleteProcThreadAttributeList(attributes);
  if (!created) return false;
  child_process_ = child.hProcess;
  CloseHandle(child.hThread);

  const std::array<HANDLE, 2> wait_handles{ready_, child_process_};
  if (WaitForMultipleObjects(static_cast<DWORD>(wait_handles.size()), wait_handles.data(), FALSE,
                             10000) != WAIT_OBJECT_0) {
    Resume();
    return false;
  }
  return active();
}

bool WatchdogClient::active() const {
  return child_process_ && done_ && WaitForSingleObject(child_process_, 0) == WAIT_TIMEOUT &&
         WaitForSingleObject(done_, 0) == WAIT_TIMEOUT &&
         host_process::IsSameProcess(host_process_handle_, host_);
}

ResumeResult WatchdogClient::Resume() {
  if (!child_process_ || !release_ || !done_) return ResumeResult::Failed;
  if (!release_requested_) {
    release_requested_ = true;
    if (!SetEvent(release_)) return ResumeResult::Failed;
  }
  const std::array<HANDLE, 2> wait_handles{done_, child_process_};
  if (WaitForMultipleObjects(static_cast<DWORD>(wait_handles.size()), wait_handles.data(), FALSE,
                             10000) != WAIT_OBJECT_0) {
    return ResumeResult::Failed;
  }
  if (WaitForSingleObject(child_process_, 1000) != WAIT_OBJECT_0) return ResumeResult::Failed;
  DWORD exit_code = STILL_ACTIVE;
  if (!GetExitCodeProcess(child_process_, &exit_code)) return ResumeResult::Failed;
  return ClassifyResumeExitCode(exit_code);
}

}  // namespace azookey::compat_test::host_hang
