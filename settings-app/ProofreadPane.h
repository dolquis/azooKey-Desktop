#pragma once

#include <memory>
#include <string>

#include "pch.h"

namespace azookey::settings {

// The "校正" pane (rich-features-spec X-3-6): the user pastes a text, the Host asks the external AI
// backend for likely mistakes, and the findings are listed. What the AI wrote (reason, suggestions)
// is shown as plain text only. Reading the text of the foreground app is the TIP's work.
class ProofreadPane : public std::enable_shared_from_this<ProofreadPane> {
 public:
  void Build(winrt::Microsoft::UI::Xaml::Controls::StackPanel panel,
             winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher);

 private:
  winrt::fire_and_forget Check();
  void RecoverFromFailure();
  void ShowStatus(winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
                  const winrt::hstring& message);
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
};

}  // namespace azookey::settings
