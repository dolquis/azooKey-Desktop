#pragma once

#include <filesystem>
#include <optional>

#include "MainWindow.g.h"
#include "SettingsDocument.h"
#include "pch.h"

namespace winrt::azookey_settings::implementation {

struct MainWindow : MainWindowT<MainWindow> {
  MainWindow();
  void ApplyLaunchArguments(std::wstring_view raw_arguments);
  winrt::fire_and_forget SaveButton_Click(Windows::Foundation::IInspectable const& sender,
                                          Microsoft::UI::Xaml::RoutedEventArgs const& args);

 private:
  winrt::fire_and_forget LoadSettingsAsync();
  Windows::Foundation::IAsyncAction LoadSettingsCoreAsync();
  Windows::Foundation::IAsyncAction SaveSettingsCoreAsync();
  void ApplySettingsToControls(const azookey::settings::SettingsDocumentResult& result);
  void ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
                  const winrt::hstring& title, const winrt::hstring& message);

  std::optional<std::filesystem::path> settings_path_;
};

}  // namespace winrt::azookey_settings::implementation

namespace winrt::azookey_settings::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::azookey_settings::factory_implementation
