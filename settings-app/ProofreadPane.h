#pragma once

#include <memory>
#include <optional>
#include <string>

#include "ProofreadSessionClient.h"
#include "pch.h"

namespace azookey::settings {

// The "校正" pane (rich-features-spec X-3-6): the user pastes a text, the Host asks the external AI
// backend for likely mistakes, and the findings are listed. What the AI wrote (reason, suggestions)
// is shown as plain text only. Reading the text of the foreground app is the TIP's work: it opens
// this pane on a session (OpenSession) that hands over the document and selects ranges in the app.
class ProofreadPane : public std::enable_shared_from_this<ProofreadPane> {
 public:
  void Build(winrt::Microsoft::UI::Xaml::Controls::StackPanel panel,
             winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher);
  // Takes over the TIP's session `token`; any earlier session is closed.
  winrt::fire_and_forget OpenSession(std::string token);

 private:
  winrt::fire_and_forget Check();
  void RecoverFromFailure();
  void ShowStatus(winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
                  const winrt::hstring& message, const wchar_t* title = nullptr);
  winrt::fire_and_forget SelectInApplication(uint32_t start, uint32_t length);
  void CloseSession();
  void UpdateCounter();

  winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox input_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock counter_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button check_button_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::InfoBar status_bar_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel results_{nullptr};
  // The text the findings refer to; the input box may have been edited since.
  std::wstring checked_text_;
  bool busy_{false};
  // The open session and the document it handed over. The text is the session's only while the
  // input box still holds it unchanged.
  std::shared_ptr<ProofreadSessionClient> session_;
  std::optional<azookey::ipc::proofread::DocumentMessage> session_document_;
  std::wstring session_text_;
  // The input box's text right after the document was put in; the box changes line breaks.
  std::wstring session_input_text_;
  // The findings came from the session's document, so their ranges are the application's.
  bool checked_session_text_{false};
};

}  // namespace azookey::settings
