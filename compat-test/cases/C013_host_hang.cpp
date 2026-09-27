// clang-format off
#include <Windows.h>
// clang-format on

#include <chrono>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <thread>

#include "azookey/ipc/Json.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "cases/HostHangWatchdogClient.h"
#include "cases/HostProcessSupport.h"
#include "runner/CompatTypes.h"

namespace azookey::compat_test {
namespace {

struct LogSnapshot {
  std::filesystem::path path;
  DWORD volume_serial{};
  DWORD file_index_high{};
  DWORD file_index_low{};
  std::uint64_t size{};
};

std::optional<LogSnapshot> TipLogSnapshot() {
  wchar_t logging[16]{};
  wchar_t level[16]{};
  if (GetEnvironmentVariableW(L"AZOOKEY_LOG", logging, static_cast<DWORD>(std::size(logging))) !=
          1 ||
      wcscmp(logging, L"1") != 0 ||
      GetEnvironmentVariableW(L"AZOOKEY_LOG_LEVEL", level, static_cast<DWORD>(std::size(level))) !=
          4 ||
      _wcsicmp(level, L"info") != 0) {
    return std::nullopt;
  }
  wchar_t local_app_data[32768]{};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data,
                                               static_cast<DWORD>(std::size(local_app_data)));
  if (length == 0 || length >= std::size(local_app_data)) return std::nullopt;
  SYSTEMTIME now{};
  GetSystemTime(&now);
  wchar_t name[32]{};
  if (swprintf_s(name, L"tip-%04u%02u%02u.jsonl", now.wYear, now.wMonth, now.wDay) < 0) {
    return std::nullopt;
  }
  LogSnapshot result;
  result.path = std::filesystem::path(local_app_data) / L"azooKey" / L"logs" / name;
  const HANDLE file = CreateFileW(result.path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return std::nullopt;
  BY_HANDLE_FILE_INFORMATION information{};
  LARGE_INTEGER size{};
  const bool valid = GetFileInformationByHandle(file, &information) && GetFileSizeEx(file, &size);
  CloseHandle(file);
  if (!valid) return std::nullopt;
  result.volume_serial = information.dwVolumeSerialNumber;
  result.file_index_high = information.nFileIndexHigh;
  result.file_index_low = information.nFileIndexLow;
  result.size = static_cast<std::uint64_t>(size.QuadPart);
  return result;
}

enum class LogEvidence { Missing, Ambiguous, Matched };

LogEvidence ReadTransitions(const LogSnapshot& before, DWORD target_process_id) {
  const auto after = TipLogSnapshot();
  if (!after || after->path != before.path || after->volume_serial != before.volume_serial ||
      after->file_index_high != before.file_index_high ||
      after->file_index_low != before.file_index_low || after->size < before.size ||
      after->size - before.size > 1024 * 1024) {
    return LogEvidence::Missing;
  }
  const HANDLE file = CreateFileW(before.path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return LogEvidence::Missing;
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(file, &information) ||
      information.dwVolumeSerialNumber != before.volume_serial ||
      information.nFileIndexHigh != before.file_index_high ||
      information.nFileIndexLow != before.file_index_low) {
    CloseHandle(file);
    return LogEvidence::Missing;
  }
  LARGE_INTEGER offset{};
  offset.QuadPart = static_cast<LONGLONG>(before.size);
  const auto byte_count = static_cast<DWORD>(after->size - before.size);
  std::string bytes(byte_count, '\0');
  DWORD read = 0;
  const bool loaded = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
                      ReadFile(file, bytes.data(), byte_count, &read, nullptr) &&
                      read == byte_count;
  CloseHandle(file);
  if (!loaded) return LogEvidence::Missing;
  bool degraded = false;
  bool ready = false;
  bool ambiguous = false;
  size_t start = 0;
  while (start < bytes.size()) {
    const size_t end = bytes.find('\n', start);
    if (end == std::string::npos) break;  // Ignore an unfinished concurrent write.
    auto record = ipc::json::Parse(std::string_view(bytes).substr(start, end - start));
    start = end + 1;
    if (!record) return LogEvidence::Ambiguous;
    if (record->GetString("component") != "tip" ||
        record->GetString("event") != "ipc_connection_state_transition") {
      continue;
    }
    const auto to = record->GetString("to");
    if (to != "degraded" && to != "ready") continue;
    const auto process_id = record->GetUInt("process_id");
    if (!process_id) {
      ambiguous = true;
      continue;
    }
    if (*process_id != target_process_id) continue;
    if (to == "degraded" && record->GetString("from") == "ready") degraded = true;
    if (to == "ready" && record->GetString("from") == "degraded" && degraded) ready = true;
  }
  if (ambiguous) return LogEvidence::Ambiguous;
  return degraded && ready ? LogEvidence::Matched : LogEvidence::Missing;
}

bool WaitForJapaneseConversion(AutomationSession& session) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (session.ClearEditor() && session.SendAscii("nihongo") && session.SendVirtualKey(VK_SPACE) &&
        session.SendVirtualKey(VK_RETURN)) {
      const auto text = session.ReadEditorText();
      if (text && text->find(L"日本語") != std::wstring::npos) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return false;
}

bool SameHostAndPipeReady(const host_process::HostProcess& host) {
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, host.process_id);
  const bool same = process && host_process::IsSameProcess(process, host);
  if (process) CloseHandle(process);
  if (!same) return false;
  azookey::ipc::NamedPipeClient client;
  const bool connected = client.Connect(azookey::ipc::DefaultPipeName(), 100);
  client.Disconnect();
  return connected;
}

}  // namespace

CaseDefinition MakeC013HostHangCase() {
  return {
      "C-013",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-013";
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
        DWORD target_process_id = 0;
        GetWindowThreadProcessId(session.window(), &target_process_id);
        if (!target_process_id || !session.ClearEditor()) {
          result.reason_code =
              target_process_id ? session.input_failure_reason() : "target-process-unavailable";
          return result;
        }
        const auto log_before = TipLogSnapshot();
        if (!log_before) {
          result.reason_code = "tip-info-log-unavailable";
          return result;
        }

        bool degraded_input = false;
        bool responsive = false;
        host_hang::ResumeResult resume_result = host_hang::ResumeResult::Failed;
        bool same_process = false;
        {
          host_hang::WatchdogClient suspension(*host);
          if (!suspension.Start()) {
            result.reason_code = "host-suspend-failed";
            return result;
          }
          const bool sent = session.SendAscii("nihongo") && session.SendVirtualKey(VK_SPACE);
          DWORD_PTR response = 0;
          responsive = SendMessageTimeoutW(session.window(), WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000,
                                           &response) != 0;
          std::this_thread::sleep_for(std::chrono::milliseconds(1200));
          const auto preedit = session.ReadEditorText();
          const bool committed = session.SendVirtualKey(VK_RETURN);
          const auto text = session.ReadEditorText();
          degraded_input = sent && committed && preedit && text &&
                           preedit->find(L"にほんご") != std::wstring::npos &&
                           text->find(L"にほんご") != std::wstring::npos;
          const bool held_through_input = suspension.active();
          resume_result = suspension.Resume();
          same_process =
              held_through_input && host_process::IsSameProcess(suspension.process(), *host);
        }
        if (resume_result == host_hang::ResumeResult::Failed) {
          result.status = ResultStatus::Fail;
          result.reason_code = "host-resume-failed";
        } else if (resume_result == host_hang::ResumeResult::ResumedByWatchdog || !same_process) {
          result.status = ResultStatus::Fail;
          result.reason_code = "host-identity-changed-or-hang-expired";
        } else if (!responsive || !degraded_input) {
          result.status = ResultStatus::Fail;
          result.reason_code = "degraded-input-unavailable";
        } else if (!WaitForJapaneseConversion(session) || !SameHostAndPipeReady(*host)) {
          result.status = ResultStatus::Fail;
          result.reason_code = "post-resume-conversion-failed";
        } else {
          const auto log_evidence = ReadTransitions(*log_before, target_process_id);
          if (log_evidence == LogEvidence::Ambiguous) {
            result.reason_code = "tip-transition-process-ambiguous";
          } else if (log_evidence == LogEvidence::Missing) {
            if (session.target_process_inherited_environment()) {
              result.status = ResultStatus::Fail;
              result.reason_code = "tip-transition-not-observed";
            } else {
              result.reason_code = "tip-info-log-not-confirmed-for-target";
            }
          } else {
            result.status = ResultStatus::Pass;
            result.reason_code = "degraded-input-same-host-and-tip-recovery-observed";
          }
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
