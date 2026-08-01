// clang-format off
#include <Windows.h>
#include <TlHelp32.h>
// clang-format on

#include <chrono>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <thread>

#include "azookey/ipc/NamedPipeTransport.h"
#include "runner/CompatTypes.h"

namespace azookey::compat_test {
namespace {

struct HostProcess {
  DWORD process_id{};
  DWORD parent_process_id{};
  std::uint64_t creation_time{};
};

std::uint64_t FileTimeValue(const FILETIME& value) {
  ULARGE_INTEGER integer{};
  integer.LowPart = value.dwLowDateTime;
  integer.HighPart = value.dwHighDateTime;
  return integer.QuadPart;
}

bool ProcessCreationTime(HANDLE process, std::uint64_t* creation_time) {
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
  *creation_time = FileTimeValue(created);
  return true;
}

bool IsExpectedSupervisor(const HostProcess& host) {
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

std::optional<HostProcess> FindHostProcess(DWORD excluded_process_id = 0) {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  std::optional<HostProcess> result;
  DWORD current_session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &current_session);
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
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
    if (!process || !ProcessCreationTime(process, &host.creation_time)) {
      if (process) CloseHandle(process);
      continue;
    }
    CloseHandle(process);
    if (!IsExpectedSupervisor(host)) continue;
    result = std::move(host);
    break;
  }
  CloseHandle(snapshot);
  return result;
}

bool IsPipeReady() {
  azookey::ipc::NamedPipeClient client;
  const bool connected = client.Connect(azookey::ipc::DefaultPipeName(), 100);
  client.Disconnect();
  return connected;
}

std::optional<HostProcess> WaitForReplacementHost(DWORD old_process_id,
                                                  std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto host = FindHostProcess(old_process_id); host && IsPipeReady()) return host;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return std::nullopt;
}

}  // namespace

CaseDefinition MakeC010HostRecoveryCase() {
  return {
      "C-010",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-010";
        result.status = ResultStatus::FailingSkip;
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        const auto host = FindHostProcess();
        if (!host) {
          result.reason_code = "inference-host-not-running";
          return result;
        }
        if (!session.ClearEditor()) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const HANDLE process =
            OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, host->process_id);
        if (!process || !TerminateProcess(process, 0) ||
            WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) {
          if (process) CloseHandle(process);
          result.reason_code = "host-termination-failed";
          return result;
        }
        CloseHandle(process);

        if (FindHostProcess(host->process_id)) {
          result.reason_code = "host-restarted-before-degraded-check";
          return result;
        }

        const bool input_sent = session.SendAscii("test");
        DWORD_PTR response = 0;
        const bool responsive = SendMessageTimeoutW(session.window(), WM_NULL, 0, 0,
                                                    SMTO_ABORTIFHUNG, 2000, &response) != 0;
        const auto text = session.ReadEditorText();

        const auto replacement = WaitForReplacementHost(host->process_id, std::chrono::seconds(15));
        if (!replacement) {
          result.status = ResultStatus::Fail;
          result.reason_code = "host-recovery-failed";
        } else if (!input_sent || !responsive || !text || text->empty()) {
          result.status = ResultStatus::Fail;
          result.reason_code = "degraded-input-unavailable";
        } else {
          result.status = ResultStatus::Pass;
          result.reason_code = "degraded-input-and-host-recovery-observed";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
