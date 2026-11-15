#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "azookey/core/ClientAction.h"
#include "azookey/ipc/Payloads.h"

namespace azookey::tsf {

// The M16 prompt window's state, kept apart from the Win32 window so it can
// be tested without one (legacy-parity-spec §4.3). Two steps: the user sends
// the prompt, sees the result, then applies it.
enum class PromptDialogState {
  // The prompt can be edited and sent.
  Editing,
  // The request is out; the prompt and the buttons but Cancel are disabled.
  Waiting,
  // A result is shown and can be applied, or the prompt sent again.
  Preview,
  // Only a fixed notice and Cancel; nothing was read or sent.
  Notice,
};

enum class PromptNotice {
  // The input field is secure: the selection was not read.
  Secure,
  // The selection is longer than the window takes.
  SelectionTooLong,
};

// What the window shows. Every text is a fixed message or the user's own text.
struct PromptDialogView {
  PromptDialogState state{PromptDialogState::Editing};
  std::wstring prompt;
  std::wstring selection;
  std::wstring result;
  std::wstring status;
  bool can_submit{false};
  bool can_apply{false};
  bool prompt_editable{false};
};

// Why the TIP has no response to show.
enum class PromptFailure {
  None,
  // No answer before the deadline.
  TimedOut,
  // The Host went away, or the answer did not parse.
  NoAnswer,
};

std::wstring_view InitialPrompt(core::PromptPreset preset) noexcept;
std::wstring_view PromptNoticeMessage(PromptNotice notice) noexcept;
// The fixed message for a failed transform (ai-backend-spec section 7.2 and
// 7.5). SafeMode has its own message, apart from the secure one.
std::wstring_view TransformFailureMessage(const std::optional<std::string>& error_class,
                                          const std::optional<std::string>& error,
                                          PromptFailure failure) noexcept;

class PromptDialogModel {
 public:
  static PromptDialogModel ForSelection(core::PromptPreset preset, std::wstring selection);
  static PromptDialogModel ForNotice(PromptNotice notice);

  // Sends `prompt` from Editing or Preview. False, with the state unchanged,
  // when the prompt is empty or too long, or nothing can be sent now.
  bool Submit(std::wstring prompt);
  // The answer to the last Submit. Ignored unless Waiting.
  void OnResponse(const std::optional<ipc::TransformSelectedTextResponse>& response,
                  PromptFailure failure);
  bool CanApply() const noexcept { return view_.can_apply; }
  // The result of the last answer, as UTF-8 from the Host.
  const std::string& result_utf8() const noexcept { return result_utf8_; }
  const std::string& prompt_utf8() const noexcept { return prompt_utf8_; }
  const PromptDialogView& view() const noexcept { return view_; }

 private:
  void Refresh();

  PromptDialogView view_;
  std::string prompt_utf8_;
  std::string result_utf8_;
};

}  // namespace azookey::tsf
