// clang-format off
#include <Windows.h>
// clang-format on

#include <chrono>
#include <optional>
#include <thread>

#include "azookey/ipc/NamedPipeTransport.h"
#include "cases/HostProcessSupport.h"
#include "runner/CompatTypes.h"

namespace azookey::compat_test {
namespace {

bool IsPipeReady() {
  azookey::ipc::NamedPipeClient client;
  const bool connected = client.Connect(azookey::ipc::DefaultPipeName(), 100);
  client.Disconnect();
  return connected;
}

std::optional<host_process::HostProcess> WaitForReplacementHost(DWORD old_process_id,
                                                                std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto host = host_process::FindHostProcess(old_process_id); host && IsPipeReady())
      return host;
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
        const auto host = host_process::FindHostProcess();
        if (!host) {
          result.reason_code = "inference-host-not-running";
          return result;
        }
        if (!session.ClearEditor()) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const HANDLE process =
            OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                        host->process_id);
        if (!process || !host_process::IsSameProcess(process, *host) ||
            !TerminateProcess(process, 0) || WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) {
          if (process) CloseHandle(process);
          result.reason_code = "host-termination-failed";
          return result;
        }
        CloseHandle(process);

        if (host_process::FindHostProcess(host->process_id)) {
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
