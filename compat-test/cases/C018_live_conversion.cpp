#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <iterator>
#include <string>
#include <thread>

#include "azookey/ipc/Json.h"
#include "runner/CompatTypes.h"

namespace azookey::compat_test {
namespace {

enum class LiveSetting { Enabled, Disabled, Unavailable };

LiveSetting ReadLiveSetting() {
  wchar_t local_app_data[32768]{};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data,
                                               static_cast<DWORD>(std::size(local_app_data)));
  if (length == 0 || length >= std::size(local_app_data)) return LiveSetting::Unavailable;
  const auto path =
      std::filesystem::path(local_app_data) / L"azooKey" / L"config" / L"settings.json";
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
               ? LiveSetting::Disabled
               : LiveSetting::Unavailable;
  }
  LARGE_INTEGER size{};
  constexpr LONGLONG kLimit = 1024 * 1024;
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > kLimit) {
    CloseHandle(file);
    return LiveSetting::Unavailable;
  }
  std::string contents(static_cast<size_t>(size.QuadPart) + 1, '\0');
  DWORD count = 0;
  const BOOL read =
      ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &count, nullptr);
  CloseHandle(file);
  if (!read || count > static_cast<size_t>(size.QuadPart)) return LiveSetting::Unavailable;
  contents.resize(count);
  const auto json = ipc::json::Parse(contents);
  if (!json || !json->IsObject()) return LiveSetting::Disabled;
  return json->GetBool("liveConversion").value_or(false) ? LiveSetting::Enabled
                                                         : LiveSetting::Disabled;
}

std::optional<std::wstring> WaitForText(AutomationSession& session, std::wstring_view expected) {
  for (int attempt = 0; attempt < 30; ++attempt) {
    auto text = session.ReadEditorText();
    if (!text || *text == expected) return text;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return session.ReadEditorText();
}

}  // namespace

CaseDefinition MakeC018LiveConversionCase() {
  return {
      "C-018",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-018";
        result.status = ResultStatus::FailingSkip;
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        const auto setting = ReadLiveSetting();
        if (setting != LiveSetting::Enabled) {
          result.reason_code = setting == LiveSetting::Disabled
                                   ? "live-conversion-disabled"
                                   : "live-conversion-setting-unavailable";
          return result;
        }
        if (!session.ClearEditor() || !session.SendAscii("nihongo")) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto preedit = WaitForText(session, L"日本語");
        if (!preedit) {
          result.reason_code = "live-preedit-unobservable";
          return result;
        }
        if (*preedit != L"日本語" || session.CandidateRect()) {
          result.status = ResultStatus::Fail;
          result.reason_code = "live-preedit-mismatch";
          return result;
        }
        if (!session.SendVirtualKey(VK_RETURN)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto committed = session.ReadEditorText();
        if (!committed) {
          result.reason_code = "live-commit-unobservable";
          return result;
        }
        if (*committed != L"日本語") {
          result.status = ResultStatus::Fail;
          result.reason_code = "live-commit-mismatch";
          return result;
        }
        if (!session.ClearEditor() || !session.SendAscii("nihongo")) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto before_backspace = WaitForText(session, L"日本語");
        if (!before_backspace) {
          result.reason_code = "live-backspace-preedit-unobservable";
          return result;
        }
        if (*before_backspace != L"日本語") {
          result.status = ResultStatus::Fail;
          result.reason_code = "live-backspace-preedit-mismatch";
          return result;
        }
        if (!session.SendVirtualKey(VK_BACK)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        auto after_backspace = session.ReadEditorText();
        for (int attempt = 0;
             after_backspace && *after_backspace == *before_backspace && attempt < 30; ++attempt) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          after_backspace = session.ReadEditorText();
        }
        if (!after_backspace) {
          result.reason_code = "live-backspace-result-unobservable";
          return result;
        }
        if (after_backspace->empty() || *after_backspace == *before_backspace) {
          result.status = ResultStatus::Fail;
          result.reason_code = "live-backspace-transition-mismatch";
          return result;
        }
        // Prediction Esc has its own meaning; close that separate window first.
        if (session.PredictionRect()) {
          if (!session.SendVirtualKey(VK_ESCAPE)) {
            result.reason_code = session.input_failure_reason();
            return result;
          }
          for (int attempt = 0; attempt < 20 && session.PredictionRect(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          if (session.PredictionRect()) {
            result.reason_code = "prediction-window-remained-before-live-escape";
            return result;
          }
        }
        if (!session.SendVirtualKey(VK_ESCAPE)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        const auto after_escape = WaitForText(session, L"");
        if (!after_escape) {
          result.reason_code = "live-escape-result-unobservable";
        } else if (!after_escape->empty()) {
          result.status = ResultStatus::Fail;
          result.reason_code = "live-escape-did-not-clear-composition";
        } else {
          result.status = ResultStatus::Pass;
          result.reason_code = "live-preedit-enter-backspace-escape-observed";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
