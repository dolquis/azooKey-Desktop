#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "LearningPane.h"
#include "MainWindow.g.h"
#include "ModelPane.h"
#include "NeologdAttribution.h"
#include "PersonaPane.h"
#include "ProfilesPane.h"
#include "ProofreadPane.h"
#include "SettingsDocument.h"
#include "SettingsFieldControls.h"
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
  void ClearSafeModeButton_Click(Windows::Foundation::IInspectable const& sender,
                                 Microsoft::UI::Xaml::RoutedEventArgs const& args);

 private:
  winrt::fire_and_forget LoadSettingsAsync();
  Windows::Foundation::IAsyncAction LoadSettingsCoreAsync();
  // True when the settings were written.
  Windows::Foundation::IAsyncOperation<bool> SaveSettingsCoreAsync();
  // What a save would be given, from the controls; `invalid_paths` lists enabled controls that
  // hold no valid value (their keys are left out of `values`).
  azookey::settings::EditableSettings SettingsFromControls(std::vector<std::string>* invalid_paths);
  // The number of keys changed from what was loaded; an invalid control counts as one.
  size_t ChangedSettingCount();
  // Recounts the changes and sets the save bar's text and the save button (section 3.2).
  void RefreshSaveBar();
  void ShowSaveStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
                      const winrt::hstring& title, const winrt::hstring& message);
  // Asks whether to save, discard or keep the changes when the window is being closed.
  winrt::fire_and_forget ConfirmCloseAsync();
  void ApplySettingsToControls(const azookey::settings::SettingsDocumentResult& result);
  void UpdateCrashReportStatus();
  void ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
                  const winrt::hstring& title, const winrt::hstring& message);
  void ShowPane(std::wstring_view tag);
  void ApplyDictionaryToControls(const azookey::settings::DictionarySettings& dictionary);
  azookey::settings::DictionarySettings DictionaryFromControls();
  void SetNeologdToggle(bool enabled);
  // Asks the Host (QueryDiagnostics) what became of the neologd layer and shows it.
  winrt::fire_and_forget RefreshNeologdLayerStatus();
  // Shows the Host's last update check (update-status.json beside settings.json, M32).
  void RefreshUpdateStatus();
  Windows::Foundation::IAsyncOperation<bool> ShowNeologdNoticesAsync(bool ask_consent);
  void ShowSafeMode(const azookey::settings::EditableSettings& settings);
  void ShowInvalidSetting(const std::string& path);

  std::optional<std::filesystem::path> settings_path_;
  // The hash of the API key the box was given when loading or last saving, to tell an edit.
  size_t loaded_api_key_hash_{std::hash<std::wstring_view>{}(std::wstring_view())};
  // The user pressed "API キーを削除" since then.
  bool openai_api_key_cleared_{false};
  std::optional<azookey::settings::NeologdPackAttribution> neologd_attribution_;
  // The neologd switch value the user has accepted; programmatic changes keep it in sync.
  bool neologd_enabled_{false};
  // Bumped for each neologd status query; an answer for an older one is dropped.
  uint64_t neologd_status_generation_{0};
  // The last state read from or written to disk, to tell which saved changes need a restart.
  azookey::settings::EditableSettings saved_settings_;
  azookey::settings::SettingsFieldControls field_controls_;
  // Cleared when the window closes; callbacks the panes keep check it before using `this`.
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
  std::shared_ptr<azookey::settings::ProfilesPane> profiles_pane_;
  std::shared_ptr<azookey::settings::ModelPane> model_pane_;
  std::shared_ptr<azookey::settings::LearningPane> learning_pane_;
  std::shared_ptr<azookey::settings::PersonaPane> persona_pane_;
  std::shared_ptr<azookey::settings::ProofreadPane> proofread_pane_;
  bool clear_safe_mode_{false};
  // Save bar state: whether the loaded file may be written, whether a save is running, whether a
  // save would repair what loading reported, and whether the controls show the loaded values.
  bool can_save_{false};
  bool saving_{false};
  bool repair_pending_{false};
  bool controls_loaded_{false};
  bool load_problem_shown_{false};
  // Set once the user chose to save or discard, so the next close is not asked about again.
  bool close_confirmed_{false};
  bool close_dialog_open_{false};
};

}  // namespace winrt::azookey_settings::implementation

namespace winrt::azookey_settings::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::azookey_settings::factory_implementation
