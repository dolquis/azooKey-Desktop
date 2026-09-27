#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

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
  std::wstring editor_name;
  std::wstring candidate_window_class;
  std::vector<std::string> cases;
  bool use_temporary_document{false};
  bool save_temporary_document_before_close{true};
  uint32_t close_grace_period_ms{1500};
  std::wstring temporary_document_extension{L".txt"};
  std::string temporary_document_contents;
  bool require_new_window{false};
  bool allow_reused_window_for_temporary_document{false};
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
  bool DismissPredictionWindow();
  bool SendAscii(const std::string& text);
  bool SendUnicode(std::wstring_view text);
  bool SendVirtualKey(WORD virtual_key);
  bool SendModifiedKey(std::initializer_list<WORD> modifiers, WORD virtual_key);
  const char* input_failure_reason() const {
    return focus_lost_                   ? "focus-lost"
           : prediction_window_remained_ ? "prediction-window-remained"
                                         : "input-injection-failed";
  }
  std::optional<std::wstring> ReadEditorText();
  std::optional<RECT> CaretRect() const;
  std::optional<RECT> CandidateRect() const;
  std::optional<RECT> PredictionRect() const;
  bool CaptureFailureArtifacts(const CaseResult& result,
                               const std::filesystem::path& output_directory) const;
  void MarkBaselineVerified() { baseline_verified_ = true; }
  bool baseline_verified() const { return baseline_verified_; }

  const TargetConfig& target() const { return target_; }
  HWND window() const { return window_; }
  bool target_process_inherited_environment() const { return window_process_is_launched_process_; }

 private:
  bool FindEditorElement();
  bool IsTargetForeground() const;
  bool SendKeyInputs(const std::vector<INPUT>& inputs);
  std::optional<RECT> WindowRectForClass(std::wstring_view class_name) const;
  void CloseLaunchedWindow();

  TargetConfig target_;
  PROCESS_INFORMATION process_info_{};
  HWND window_{nullptr};
  IUIAutomation* automation_{nullptr};
  IUIAutomationElement* editor_{nullptr};
  bool owns_window_{false};
  bool owns_document_tab_{false};
  bool owns_launched_process_{false};
  bool window_process_is_launched_process_{false};
  bool remove_temporary_document_on_destroy_{true};
  bool baseline_verified_{false};
  bool focus_lost_{false};
  bool prediction_window_remained_{false};
  uint64_t editor_discovery_duration_ms_{0};
  std::filesystem::path temporary_document_;
};

struct CaseDefinition {
  std::string id;
  CaseResult (*run)(AutomationSession& session);
  // C-001 の変換成功を前提にするケース。前提を持たないケースは false を明示する。
  bool requires_baseline{true};
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
CaseDefinition MakeC013HostHangCase();
CaseDefinition MakeC014ExtraRomajiCase();
CaseDefinition MakeC015CandidateKeysCase();
CaseDefinition MakeC016FastInputCase();
CaseDefinition MakeC017ControlBackspaceCase();
CaseDefinition MakeC018LiveConversionCase();
CaseDefinition MakeC019PredictionWindowCase();

const char* ResultStatusName(ResultStatus status);

}  // namespace azookey::compat_test
