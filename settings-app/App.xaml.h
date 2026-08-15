#pragma once

#include "pch.h"
#include "App.xaml.g.h"

namespace winrt::azookey_settings::implementation {

struct App : AppT<App> {
  App();
  ~App();

  void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

 private:
  HANDLE instance_mutex_ = nullptr;
  Microsoft::UI::Xaml::Window window_{nullptr};
};

}  // namespace winrt::azookey_settings::implementation
