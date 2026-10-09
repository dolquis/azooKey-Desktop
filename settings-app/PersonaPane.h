#pragma once

#include <memory>

#include "pch.h"

namespace azookey::settings {

// The "Persona" pane (rich-features-spec X-2-7): the four ratios the Host computed from the
// learning data, read-only. The Host sends ratios and a sample count only, never a surface.
class PersonaPane : public std::enable_shared_from_this<PersonaPane> {
 public:
  void Build(winrt::Microsoft::UI::Xaml::Controls::StackPanel panel,
             winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher);
  // Called each time the pane is shown; reads the Host's cached ratios until one read succeeds.
  void OnShown();

 private:
  winrt::fire_and_forget Refresh();
  void RecoverFromFailure();
  void ShowProblem(const winrt::hstring& message);

  winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::InfoBar problem_bar_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button refresh_button_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel content_{nullptr};
  bool loaded_once_{false};
  bool loading_{false};
};

}  // namespace azookey::settings
