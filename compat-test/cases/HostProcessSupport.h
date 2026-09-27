#pragma once

// clang-format off
#include <Windows.h>
#include <TlHelp32.h>
// clang-format on

#include <cstdint>
#include <cwchar>
#include <iterator>
#include <optional>

namespace azookey::compat_test::host_process {

struct HostProcess {
  DWORD process_id{};
  DWORD parent_process_id{};
  std::uint64_t creation_time{};
};

inline std::uint64_t FileTimeValue(const FILETIME& value) {
  ULARGE_INTEGER integer{};
  integer.LowPart = value.dwLowDateTime;
  integer.HighPart = value.dwHighDateTime;
  return integer.QuadPart;
}

inline bool ProcessCreationTime(HANDLE process, std::uint64_t* creation_time) {
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
  *creation_time = FileTimeValue(created);
  return true;
}

inline bool IsExpectedSupervisor(const HostProcess& host) {
  const HANDLE parent =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, host.parent_process_id);
  if (!parent) return false;
  bool valid = WaitForSingleObject(parent, 0) == WAIT_TIMEOUT;
  std::uint64_t parent_creation_time = 0;
  valid = valid && ProcessCreationTime(parent, &parent_creation_time) &&
          parent_creation_time < host.creation_time;
  wchar_t image_path[32768]{};
  DWORD length = static_cast<DWORD>(std::size(image_path));
  valid = valid && QueryFullProcessImageNameW(parent, 0, image_path, &length) != FALSE;
  CloseHandle(parent);
  if (!valid) return false;
  const wchar_t* file_name = std::wcsrchr(image_path, L'\\');
  file_name = file_name ? file_name + 1 : image_path;
  return _wcsicmp(file_name, L"pwsh.exe") == 0 || _wcsicmp(file_name, L"powershell.exe") == 0;
}

inline bool IsSameProcess(HANDLE process, const HostProcess& host) {
  std::uint64_t creation_time = 0;
  return GetProcessId(process) == host.process_id &&
         WaitForSingleObject(process, 0) == WAIT_TIMEOUT &&
         ProcessCreationTime(process, &creation_time) && creation_time == host.creation_time &&
         IsExpectedSupervisor(host);
}

inline std::optional<HostProcess> FindHostProcess(DWORD excluded_process_id = 0) {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  std::optional<HostProcess> result;
  DWORD current_session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &current_session)) {
    CloseHandle(snapshot);
    return std::nullopt;
  }
  for (BOOL found = Process32FirstW(snapshot, &entry); found;
       found = Process32NextW(snapshot, &entry)) {
    if (entry.th32ProcessID == excluded_process_id ||
        _wcsicmp(entry.szExeFile, L"azookey_inference_host.exe") != 0) {
      continue;
    }
    DWORD process_session = 0;
    if (!ProcessIdToSessionId(entry.th32ProcessID, &process_session) ||
        process_session != current_session) {
      continue;
    }
    HostProcess host{entry.th32ProcessID, entry.th32ParentProcessID, 0};
    const HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID);
    if (!process || !ProcessCreationTime(process, &host.creation_time) ||
        WaitForSingleObject(process, 0) != WAIT_TIMEOUT) {
      if (process) CloseHandle(process);
      continue;
    }
    CloseHandle(process);
    if (!IsExpectedSupervisor(host)) continue;
    result = host;
    break;
  }
  CloseHandle(snapshot);
  return result;
}

}  // namespace azookey::compat_test::host_process
