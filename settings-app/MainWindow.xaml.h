#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "MainWindow.g.h"
#include "NeologdAttribution.h"
#include "SettingsDocument.h"
#include "pch.h"

namespace winrt::azookey_settings::implementation {

struct MainWindow : MainWindowT<MainWindow> {
  MainWindow();
  void ApplyLaunchArguments(std::wstring_view raw_arguments);
  winrt::fire_and_forget SaveButton_Click(Windows::Foundation::IInspectable const& sender,
                                          Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OpenAiApiKeyPasswordBox_PasswordChanged(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void ClearOpenAiApiKeyButton_Click(Windows::Foundation::IInspectable const& sender,
                                    Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OpenCrashReportsButton_Click(Windows::Foundation::IInspectable const& sender,
                                    Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OpenThirdPartyNoticesButton_Click(Windows::Foundation::IInspectable const& sender,
                                         Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void SettingsNavigationView_SelectionChanged(
      Microsoft::UI::Xaml::Controls::NavigationView const& sender,
      Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args);
  winrt::fire_and_forget NeologdDictionaryToggle_Toggled(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  winrt::fire_and_forget ShowNeologdNoticesButton_Click(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);

 private:
  winrt::fire_and_forget LoadSettingsAsync();
  Windows::Foundation::IAsyncAction LoadSettingsCoreAsync();
  Windows::Foundation::IAsyncAction SaveSettingsCoreAsync();
  void ApplySettingsToControls(const azookey::settings::SettingsDocumentResult& result);
  void UpdateCrashReportStatus();
  void ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
                  const winrt::hstring& title, const winrt::hstring& message);
  void ShowPane(std::wstring_view tag);
  void ApplyDictionaryToControls(const azookey::settings::DictionarySettings& dictionary);
  azookey::settings::DictionarySettings DictionaryFromControls();
  void SetNeologdToggle(bool enabled);
  Windows::Foundation::IAsyncOperation<bool> ShowNeologdNoticesAsync(bool ask_consent);

  std::optional<std::filesystem::path> settings_path_;
  bool openai_api_key_changed_{false};
  std::optional<azookey::settings::NeologdPackAttribution> neologd_attribution_;
  // The neologd switch value the user has accepted; programmatic changes keep it in sync.
  bool neologd_enabled_{false};
  // The last state read from or written to disk, to tell which saved changes need a restart.
  azookey::settings::EditableSettings saved_settings_;
};

}  // namespace winrt::azookey_settings::implementation

namespace winrt::azookey_settings::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::azookey_settings::factory_implementation
