#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#ifdef GetObject
#undef GetObject
#endif

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct IUIAutomation;
struct IUIAutomationElement;

namespace azookey::compat_test {

enum class ResultStatus {
  Pass,
  Fail,
  FailingSkip,
};

struct CaseResult {
  std::string id;
  ResultStatus status{ResultStatus::FailingSkip};
  std::string reason_code;
  uint64_t duration_ms{0};
  std::filesystem::path failure_directory;
};

struct TargetConfig {
  std::string id;
  std::string display_name;
  std::string app_id;
  std::string automation_level;
  std::wstring executable;
  std::vector<std::wstring> arguments;
  std::vector<std::wstring> window_classes;
  std::wstring edit_control_class;
  std::string editor_control_type{"document"};
  std::wstring candidate_window_class;
  std::vector<std::string> cases;
  bool use_temporary_document{false};
  bool save_temporary_document_before_close{true};
  std::wstring temporary_document_extension{L".txt"};
  std::string temporary_document_contents;
};

class AutomationSession {
 public:
  explicit AutomationSession(TargetConfig target);
  ~AutomationSession();

  AutomationSession(const AutomationSession&) = delete;
  AutomationSession& operator=(const AutomationSession&) = delete;

  bool Start(std::string* reason_code);
  bool FocusEditor();
  bool ClearEditor();
  bool SendAscii(const std::string& text);
  bool SendUnicode(std::wstring_view text);
  bool SendVirtualKey(WORD virtual_key);
  bool SendModifiedKey(std::initializer_list<WORD> modifiers, WORD virtual_key);
  const char* input_failure_reason() const {
    return focus_lost_ ? "focus-lost" : "input-injection-failed";
  }
  std::optional<std::wstring> ReadEditorText();
  std::optional<RECT> CaretRect() const;
  std::optional<RECT> CandidateRect() const;
  bool CaptureFailureArtifacts(const CaseResult& result,
                               const std::filesystem::path& output_directory) const;
  void MarkBaselineVerified() { baseline_verified_ = true; }
  bool baseline_verified() const { return baseline_verified_; }

  const TargetConfig& target() const { return target_; }
  HWND window() const { return window_; }

 private:
  bool FindEditorElement();
  bool IsTargetForeground() const;
  bool SendKeyInputs(const std::vector<INPUT>& inputs);
  void CloseLaunchedWindow();

  TargetConfig target_;
  PROCESS_INFORMATION process_info_{};
  HWND window_{nullptr};
  IUIAutomation* automation_{nullptr};
  IUIAutomationElement* editor_{nullptr};
  bool owns_window_{false};
  bool baseline_verified_{false};
  bool focus_lost_{false};
  std::filesystem::path temporary_document_;
};

struct CaseDefinition {
  std::string id;
  CaseResult (*run)(AutomationSession& session);
};

CaseDefinition MakeC001BasicInputCase();
CaseDefinition MakeC002BackspaceCase();
CaseDefinition MakeC003EscapeCase();
CaseDefinition MakeC004CandidatePositionCase();
CaseDefinition MakeC005MonitorClampCase();
CaseDefinition MakeC006DpiScalingCase();
CaseDefinition MakeC007SurrogatePairCase();
CaseDefinition MakeC008UndoRedoCase();
CaseDefinition MakeC009FocusTransitionCase();
CaseDefinition MakeC010HostRecoveryCase();
CaseDefinition MakeC011ShortcutRoutingCase();
CaseDefinition MakeC012RomanizationCase();

const char* ResultStatusName(ResultStatus status);

}  // namespace azookey::compat_test
