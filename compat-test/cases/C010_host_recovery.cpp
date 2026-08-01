// clang-format off
#include <Windows.h>
#include <TlHelp32.h>
// clang-format on

#include <chrono>
#include <cwchar>
#include <optional>
#include <thread>
#include <vector>

#include "runner/CompatTypes.h"

namespace azookey::compat_test {
namespace {

struct HostProcess {
  DWORD process_id{};
  DWORD parent_process_id{};
  std::wstring image_path;
};

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
    HostProcess host{entry.th32ProcessID, entry.th32ParentProcessID, {}};
    const HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
    if (process) {
      DWORD length = 32768;
      host.image_path.resize(length);
      if (QueryFullProcessImageNameW(process, 0, host.image_path.data(), &length)) {
        host.image_path.resize(length);
      } else {
        host.image_path.clear();
      }
      CloseHandle(process);
    }
    result = std::move(host);
    break;
  }
  CloseHandle(snapshot);
  return result;
}

bool IsProcessAlive(DWORD process_id) {
  const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, process_id);
  if (!process) return false;
  const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  return alive;
}

std::optional<HostProcess> WaitForReplacementHost(DWORD old_process_id,
                                                  std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto host = FindHostProcess(old_process_id)) return host;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return std::nullopt;
}

bool StartHostFallback(const std::wstring& image_path) {
  if (image_path.empty()) return false;
  std::wstring command = L"\"" + image_path + L"\"";
  std::vector<wchar_t> writable(command.begin(), command.end());
  writable.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, writable.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &startup, &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

}  // namespace

CaseDefinition MakeC010HostRecoveryCase() {
  return {
      "C-010",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-010";
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        const auto host = FindHostProcess();
        if (!host) {
          result.reason_code = "inference-host-not-running";
          return result;
        }
        if (!IsProcessAlive(host->parent_process_id)) {
          result.reason_code = "host-supervisor-unavailable";
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

        const auto input_started = std::chrono::steady_clock::now();
        const bool input_sent = session.SendAscii("test");
        DWORD_PTR response = 0;
        const bool responsive = SendMessageTimeoutW(session.window(), WM_NULL, 0, 0,
                                                    SMTO_ABORTIFHUNG, 2000, &response) != 0;
        const auto text = session.ReadEditorText();
        const auto input_duration = std::chrono::steady_clock::now() - input_started;

        auto replacement = WaitForReplacementHost(host->process_id, std::chrono::seconds(15));
        if (!replacement && StartHostFallback(host->image_path)) {
          replacement = WaitForReplacementHost(host->process_id, std::chrono::seconds(5));
        }
        if (!replacement) {
          result.status = ResultStatus::Fail;
          result.reason_code = "host-recovery-failed";
        } else if (!input_sent || !responsive || !text || text->empty()) {
          result.status = ResultStatus::Fail;
          result.reason_code = "degraded-input-unavailable";
        } else if (input_duration > std::chrono::seconds(3)) {
          result.status = ResultStatus::Fail;
          result.reason_code = "degraded-input-timeout";
        } else {
          result.status = ResultStatus::Pass;
          result.reason_code = "degraded-input-and-host-recovery-observed";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
