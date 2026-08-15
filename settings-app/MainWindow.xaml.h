#pragma once

#include "MainWindow.g.h"
#include "pch.h"

namespace winrt::azookey_settings::implementation {

struct MainWindow : MainWindowT<MainWindow> {
  MainWindow();
  void ApplyLaunchArguments(std::wstring_view raw_arguments);
};

}  // namespace winrt::azookey_settings::implementation

namespace winrt::azookey_settings::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::azookey_settings::factory_implementation
