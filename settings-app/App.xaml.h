#pragma once

#include "App.xaml.g.h"
#include "pch.h"

namespace winrt::azookey_settings::implementation {

struct App : AppT<App> {
  App();
  ~App();

  void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

 private:
  winrt::fire_and_forget LaunchAsync();
  void OnActivated(Windows::Foundation::IInspectable const&,
                   Microsoft::Windows::AppLifecycle::AppActivationArguments const& arguments);
  void ApplyActivation(Microsoft::Windows::AppLifecycle::AppActivationArguments const& arguments);
  void RestoreAndActivateWindow();

  Microsoft::Windows::AppLifecycle::AppInstance app_instance_{nullptr};
  winrt::event_token activated_token_{};
  Microsoft::UI::Dispatching::DispatcherQueue dispatcher_queue_{nullptr};
  Microsoft::UI::Xaml::Window window_{nullptr};
};

}  // namespace winrt::azookey_settings::implementation
