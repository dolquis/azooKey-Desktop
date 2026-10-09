// C++/WinRT requires the precompiled header before generated XAML headers.
// clang-format off
#include "pch.h"
#include "MainWindow.xaml.h"
#include "LaunchArguments.h"
#include "SettingsDocument.h"
#include "SettingsIpcClient.h"
#include "StatusPaneModel.h"
#include "UiDispatch.h"
#include "ModelPaneModel.h"
// clang-format on

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <coroutine>
#include <cwctype>
#include <filesystem>
#include <string>

#include "azookey/core/CrashReporting.h"

namespace {

using azookey::settings::ResumeForeground;

winrt::hstring LoadProblemMessage(
    const azookey::settings::SettingsDocumentResult& result,
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader const& resources) {
  switch (result.status) {
    case azookey::settings::SettingsDocumentStatus::LockUnavailable:
      return resources.GetString(L"SettingsBusyMessage");
    case azookey::settings::SettingsDocumentStatus::Invalid:
      return resources.GetString(L"SettingsInvalidMessage");
    case azookey::settings::SettingsDocumentStatus::ReadError:
      return resources.GetString(L"SettingsReadFailedMessage");
    default:
      return resources.GetString(L"InvalidEntriesSkipped");
  }
}

void AppendMessage(winrt::hstring* message, const winrt::hstring& addition) {
  if (!message->empty()) *message = *message + L"\n";
  *message = *message + addition;
}

winrt::hstring StatusText(
    azookey::settings::LaunchArgumentStatus status,
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader const& resources) {
  switch (status) {
    case azookey::settings::LaunchArgumentStatus::NotSupplied:
      return resources.GetString(L"NotSuppliedValue");
    case azookey::settings::LaunchArgumentStatus::Invalid:
      return resources.GetString(L"InvalidValue");
    case azookey::settings::LaunchArgumentStatus::Valid:
      break;
  }
  return {};
}

}  // namespace

namespace winrt::azookey_settings::implementation {

MainWindow::MainWindow() {
  InitializeComponent();
  SaveButton().IsEnabled(false);
  field_controls_.Build([this](azookey::settings::SettingsPane pane) {
    using Pane = azookey::settings::SettingsPane;
    switch (pane) {
      case Pane::General:
        return GeneralFieldsPanel();
      case Pane::Input:
        return InputFieldsPanel();
      case Pane::Dictionary:
        return DictionaryFieldsPanel();
      case Pane::Ai:
        return AiFieldsPanel();
      case Pane::Privacy:
        return PrivacyFieldsPanel();
      case Pane::Advanced:
        break;
    }
    return AdvancedFieldsPanel();
  });
  // The panes keep these callbacks for requests that outlive a click, so they must not touch the
  // window once it has closed.
  Closed([alive = alive_](auto const&, auto const&) { *alive = false; });
  profiles_pane_ = std::make_shared<azookey::settings::ProfilesPane>();
  profiles_pane_->Build(ProfilesPaneHost(), {[this, alive = alive_] {
                          if (!*alive) throw winrt::hresult_error(E_ABORT);
                          return Content().XamlRoot();
                        }});
  model_pane_ = std::make_shared<azookey::settings::ModelPane>();
  model_pane_->Build(ModelsPaneHost(), DispatcherQueue(),
                     {[this, alive = alive_] {
                        return *alive ? winrt::to_string(ModelPathTextBox().Text()) : std::string();
                      },
                      [this, alive = alive_](const std::string& path) {
                        if (*alive) ModelPathTextBox().Text(winrt::to_hstring(path));
                      }});
  learning_pane_ = std::make_shared<azookey::settings::LearningPane>();
  learning_pane_->Build(LearningPaneHost(), DispatcherQueue(),
                        {[this, alive = alive_] {
                           if (!*alive) throw winrt::hresult_error(E_ABORT);
                           return Content().XamlRoot();
                         },
                         [this, alive = alive_] {
                           HWND window = nullptr;
                           if (*alive) {
                             if (const auto native = this->try_as<::IWindowNative>()) {
                               native->get_WindowHandle(&window);
                             }
                           }
                           return window;
                         }});
  persona_pane_ = std::make_shared<azookey::settings::PersonaPane>();
  persona_pane_->Build(PersonaPaneHost(), DispatcherQueue());
  proofread_pane_ = std::make_shared<azookey::settings::ProofreadPane>();
  proofread_pane_->Build(ProofreadPaneHost(), DispatcherQueue());
  SettingsNavigationView().SelectedItem(GeneralNavigationItem());
  neologd_attribution_ = azookey::settings::ParseNeologdPackAttribution(
      azookey::settings::PinnedNeologdPackManifestJson());
  if (!neologd_attribution_ || !neologd_attribution_->published) {
    Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
    NeologdPackStatusText().Text(resources.GetString(
        neologd_attribution_ ? L"NeologdPackUnpublished" : L"NeologdNoticesUnavailable"));
    NeologdPackStatusText().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
    ShowNeologdNoticesButton().IsEnabled(neologd_attribution_.has_value());
  }
  settings_path_ = azookey::settings::DefaultSettingsPath();
  LoadSettingsAsync();
}

void MainWindow::SettingsNavigationView_SelectionChanged(
    Microsoft::UI::Xaml::Controls::NavigationView const&,
    Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args) {
  if (const auto item =
          args.SelectedItem().try_as<Microsoft::UI::Xaml::Controls::NavigationViewItem>()) {
    ShowPane(winrt::unbox_value_or<winrt::hstring>(item.Tag(), L""));
  }
}

void MainWindow::ShowPane(std::wstring_view tag) {
  const auto visible = [tag](std::wstring_view pane) {
    return tag == pane ? Microsoft::UI::Xaml::Visibility::Visible
                       : Microsoft::UI::Xaml::Visibility::Collapsed;
  };
  GeneralPane().Visibility(visible(L"General"));
  InputPane().Visibility(visible(L"Input"));
  DictionaryPane().Visibility(visible(L"Dictionary"));
  AiPane().Visibility(visible(L"Ai"));
  PrivacyPane().Visibility(visible(L"Privacy"));
  ProfilesPaneHost().Visibility(visible(L"Profiles"));
  ModelsPaneHost().Visibility(visible(L"Models"));
  LearningPaneHost().Visibility(visible(L"Learning"));
  PersonaPaneHost().Visibility(visible(L"Persona"));
  ProofreadPaneHost().Visibility(visible(L"Proofread"));
  AdvancedPane().Visibility(visible(L"Advanced"));
  VersionPane().Visibility(visible(L"Version"));
  if (tag == L"Models" && model_pane_) model_pane_->OnShown();
  if (tag == L"Learning" && learning_pane_) learning_pane_->OnShown();
  if (tag == L"Persona" && persona_pane_) persona_pane_->OnShown();
  if (tag == L"Dictionary") RefreshNeologdLayerStatus();
}

void MainWindow::ApplyDictionaryToControls(
    const azookey::settings::DictionarySettings& dictionary) {
  SudachiDictionaryToggle().IsOn(dictionary.sudachi_enabled);
  NamedEntityDictionaryToggle().IsOn(dictionary.named_entity_enabled);
  TechnicalTermsDictionaryToggle().IsOn(dictionary.technical_terms_enabled);
  UserDictionaryToggle().IsOn(dictionary.user_dictionary_enabled);
  AutoWordsDictionaryToggle().IsOn(dictionary.auto_words_enabled);
  AppSpecificDictionaryToggle().IsOn(dictionary.app_specific_dictionary_enabled);
  SetNeologdToggle(dictionary.neologd_enabled);
  // Enabled only once the stored value is known, so a consent dialog never races the load.
  NeologdDictionaryToggle().IsEnabled(true);
}

azookey::settings::DictionarySettings MainWindow::DictionaryFromControls() {
  azookey::settings::DictionarySettings dictionary;
  dictionary.sudachi_enabled = SudachiDictionaryToggle().IsOn();
  dictionary.neologd_enabled = neologd_enabled_;
  dictionary.named_entity_enabled = NamedEntityDictionaryToggle().IsOn();
  dictionary.technical_terms_enabled = TechnicalTermsDictionaryToggle().IsOn();
  dictionary.user_dictionary_enabled = UserDictionaryToggle().IsOn();
  dictionary.auto_words_enabled = AutoWordsDictionaryToggle().IsOn();
  dictionary.app_specific_dictionary_enabled = AppSpecificDictionaryToggle().IsOn();
  return dictionary;
}

void MainWindow::SetNeologdToggle(bool enabled) {
  // Record the value first so the Toggled handler sees no transition to consent to.
  neologd_enabled_ = enabled;
  NeologdDictionaryToggle().IsOn(enabled);
}

winrt::fire_and_forget MainWindow::NeologdDictionaryToggle_Toggled(
    Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
  const auto lifetime = get_strong();
  const bool requested = NeologdDictionaryToggle().IsOn();
  if (!azookey::settings::NeologdConsentRequired(neologd_enabled_, requested)) {
    neologd_enabled_ = requested;
    RefreshNeologdLayerStatus();
    co_return;
  }
  // auto-word-registration-spec section 14.9: the upstream license and attribution are shown,
  // and the layer is enabled only after the user accepts them.
  bool accepted = false;
  if (neologd_attribution_) {
    try {
      accepted = co_await ShowNeologdNoticesAsync(true);
    } catch (...) {
      // For example another dialog is already open; say why the switch went back off.
      accepted = false;
      Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
      ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error,
                 resources.GetString(L"NeologdNoticesTitle"),
                 resources.GetString(L"NeologdNoticesDialogFailed"));
    }
  } else {
    Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
    ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error,
               resources.GetString(L"NeologdNoticesTitle"),
               resources.GetString(L"NeologdNoticesUnavailable"));
  }
  SetNeologdToggle(accepted);
  RefreshNeologdLayerStatus();
}

winrt::fire_and_forget MainWindow::RefreshNeologdLayerStatus() {
  const auto lifetime = get_strong();
  const auto dispatcher = DispatcherQueue();
  const auto generation = ++neologd_status_generation_;
  try {
    co_await winrt::resume_background();
    const auto result =
        azookey::settings::RequestQueryDiagnostics(azookey::settings::DefaultSettingsIpcOptions());
    co_await ResumeForeground(dispatcher);
    if (!*alive_ || generation != neologd_status_generation_) co_return;

    Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
    winrt::hstring text;
    if (result.status != azookey::settings::HostCallStatus::Ok) {
      text = resources.GetString(
          winrt::to_hstring(azookey::settings::HostCallStatusResource(result.status)));
    } else {
      const bool saved_enabled =
          saved_settings_.dictionary && saved_settings_.dictionary->neologd_enabled;
      const auto kind = azookey::settings::ClassifyNeologdStatus(result.response->neologd_layer,
                                                                 saved_enabled, neologd_enabled_);
      text = resources.GetString(winrt::to_hstring(azookey::settings::NeologdStatusResource(kind)));
      if (kind == azookey::settings::NeologdStatusKind::Error &&
          result.response->neologd_layer->reason) {
        text = text + L" (" + winrt::to_hstring(*result.response->neologd_layer->reason) + L")";
      }
    }
    NeologdLayerStatusText().Text(text);
    NeologdLayerStatusText().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
  } catch (...) {
    // The status line is informational; a failed query leaves the last text.
  }
}

winrt::fire_and_forget MainWindow::ShowNeologdNoticesButton_Click(
    Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
  const auto lifetime = get_strong();
  try {
    co_await ShowNeologdNoticesAsync(false);
  } catch (...) {
    co_return;
  }
}

Windows::Foundation::IAsyncOperation<bool> MainWindow::ShowNeologdNoticesAsync(bool ask_consent) {
  const auto lifetime = get_strong();
  if (!neologd_attribution_) co_return false;
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  Microsoft::UI::Xaml::Controls::StackPanel panel;
  panel.Spacing(12);
  const auto append_text = [&panel](const winrt::hstring& text) {
    Microsoft::UI::Xaml::Controls::TextBlock block;
    block.Text(text);
    block.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    panel.Children().Append(block);
  };
  if (!neologd_attribution_->published) append_text(resources.GetString(L"NeologdPackUnpublished"));
  append_text(resources.GetString(ask_consent ? L"NeologdConsentIntro" : L"NeologdNoticesIntro"));

  // Shown verbatim from the pinned manifest (section 15.6 keeps notices unedited).
  Microsoft::UI::Xaml::Controls::TextBlock notices;
  notices.Text(winrt::to_hstring(neologd_attribution_->notices));
  notices.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
  notices.IsTextSelectionEnabled(true);
  Microsoft::UI::Xaml::Automation::AutomationProperties::SetAutomationId(notices,
                                                                         L"NeologdNoticesText");
  Microsoft::UI::Xaml::Controls::ScrollViewer scroller;
  scroller.MaxHeight(360);
  scroller.Content(notices);
  panel.Children().Append(scroller);

  Microsoft::UI::Xaml::Controls::ContentDialog dialog;
  dialog.XamlRoot(Content().XamlRoot());
  dialog.Title(winrt::box_value(resources.GetString(L"NeologdNoticesTitle")));
  dialog.Content(panel);
  if (ask_consent) {
    dialog.PrimaryButtonText(resources.GetString(L"NeologdConsentAcceptButton"));
    dialog.CloseButtonText(resources.GetString(L"NeologdConsentCancelButton"));
  } else {
    dialog.CloseButtonText(resources.GetString(L"NeologdNoticesCloseButton"));
  }
  dialog.DefaultButton(Microsoft::UI::Xaml::Controls::ContentDialogButton::Close);
  const auto result = co_await dialog.ShowAsync();
  co_return result == Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary;
}

winrt::fire_and_forget MainWindow::LoadSettingsAsync() {
  try {
    co_await LoadSettingsCoreAsync();
  } catch (...) {
    co_return;
  }
}

Windows::Foundation::IAsyncAction MainWindow::LoadSettingsCoreAsync() {
  const auto lifetime = get_strong();
  if (!settings_path_) {
    Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
    ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error,
               resources.GetString(L"LoadFailedTitle"),
               resources.GetString(L"LocalAppDataUnavailable"));
    SaveButton().IsEnabled(false);
    co_return;
  }

  const auto path = *settings_path_;
  const auto dispatcher = DispatcherQueue();
  co_await winrt::resume_background();
  const auto result = azookey::settings::LoadSettingsDocument(path);
  co_await ResumeForeground(dispatcher);
  ApplySettingsToControls(result);
}

void MainWindow::ApplySettingsToControls(const azookey::settings::SettingsDocumentResult& result) {
  const bool collect_crash_metadata = result.settings.crash_report_consent == "local";
  CrashReportConsentToggle().IsOn(collect_crash_metadata);
  azookey::core::CrashReporting::SetConsent(collect_crash_metadata
      ? azookey::core::CrashConsent::Local : azookey::core::CrashConsent::Off);
  UpdateCrashReportStatus();
  SaveButton().IsEnabled(result.status !=
                             azookey::settings::SettingsDocumentStatus::LockUnavailable &&
                         result.status != azookey::settings::SettingsDocumentStatus::ReadError);
  saved_settings_ = result.settings;
  saved_settings_.openai_api_key.clear();  // Only compared for restart; keep no extra secret copy.
  ApplyDictionaryToControls(
      result.settings.dictionary.value_or(azookey::settings::DictionarySettings{}));
  field_controls_.Apply(result.settings.values);
  ShowSafeMode(result.settings);
  ModelEnabledToggle().IsOn(result.settings.model_enabled);
  ModelPathTextBox().Text(winrt::to_hstring(result.settings.model_selected_path));
  model_pane_->SetHistory(result.settings.benchmark_history);
  // The stored switch decides how a not_requested layer reads, so ask again now it is known.
  if (DictionaryPane().Visibility() == Microsoft::UI::Xaml::Visibility::Visible) {
    RefreshNeologdLayerStatus();
  }
  profiles_pane_->SetProfiles(
      result.settings.profiles_by_app.value_or(azookey::settings::AppProfiles{}),
      result.settings.legacy_prompt_prefixes);
  OpenAiApiKeyPasswordBox().Password(winrt::to_hstring(result.settings.openai_api_key));
  openai_api_key_changed_ = false;
  if (result.settings.model_backend_preference) {
    const auto& backend = *result.settings.model_backend_preference;
    BackendPreferenceCombo().SelectedIndex(backend == "cpu" ? 1 : 0);
    UnsupportedBackendText().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
  } else {
    BackendPreferenceCombo().SelectedIndex(-1);
    Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
    const auto message = resources.GetString(L"UnsupportedBackendPrefix") +
                         winrt::to_hstring(result.settings.hidden_backend_preference) + L"\n" +
                         resources.GetString(L"BackendDowngradeNote");
    UnsupportedBackendText().Text(message);
    UnsupportedBackendText().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
  }

  const auto& log_level = result.settings.log_level;
  LogLevelCombo().SelectedIndex(log_level == "error"   ? 0
                                : log_level == "warn"  ? 1
                                : log_level == "debug" ? 3
                                                       : 2);

  if (result.error || !result.warnings.empty()) {
    Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
    const auto message = LoadProblemMessage(result, resources);
    const auto severity =
        result.status == azookey::settings::SettingsDocumentStatus::LockUnavailable ||
                result.status == azookey::settings::SettingsDocumentStatus::ReadError
            ? Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error
            : Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning;
    ShowStatus(severity, resources.GetString(L"LoadFailedTitle"), message);
  }
}

void MainWindow::ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
                            const winrt::hstring& title, const winrt::hstring& message) {
  SettingsStatusInfoBar().Severity(severity);
  SettingsStatusInfoBar().Title(title);
  SettingsStatusInfoBar().Message(message);
  SettingsStatusInfoBar().IsOpen(true);
}

winrt::fire_and_forget MainWindow::SaveButton_Click(Windows::Foundation::IInspectable const&,
                                                    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    co_await SaveSettingsCoreAsync();
  } catch (...) {
    co_return;
  }
}

void MainWindow::OpenAiApiKeyPasswordBox_PasswordChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  openai_api_key_changed_ = true;
}

void MainWindow::ClearOpenAiApiKeyButton_Click(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  OpenAiApiKeyPasswordBox().Password(L"");
  openai_api_key_changed_ = true;
}

Windows::Foundation::IAsyncAction MainWindow::SaveSettingsCoreAsync() {
  const auto lifetime = get_strong();
  if (!settings_path_) {
    Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
    ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error,
               resources.GetString(L"SaveFailedTitle"),
               resources.GetString(L"LocalAppDataUnavailable"));
    co_return;
  }

  azookey::settings::EditableSettings settings;
  settings.crash_report_consent = CrashReportConsentToggle().IsOn() ? "local" : "off";
  settings.model_enabled = ModelEnabledToggle().IsOn();
  const int backend_index = BackendPreferenceCombo().SelectedIndex();
  if (backend_index == 0 || backend_index == 1) {
    settings.model_backend_preference = backend_index == 1 ? "cpu" : "auto";
  } else {
    settings.model_backend_preference.reset();
  }
  settings.model_selected_path = winrt::to_string(ModelPathTextBox().Text());
  settings.openai_api_key_changed = openai_api_key_changed_;
  if (openai_api_key_changed_) {
    settings.openai_api_key = winrt::to_string(OpenAiApiKeyPasswordBox().Password());
  }
  const int log_index = LogLevelCombo().SelectedIndex();
  settings.log_level = log_index == 0   ? "error"
                       : log_index == 1 ? "warn"
                       : log_index == 3 ? "debug"
                                        : "info";
  settings.dictionary = DictionaryFromControls();
  settings.dictionary_loaded = saved_settings_.dictionary;
  settings.profiles_by_app = profiles_pane_->Profiles();
  settings.profiles_by_app_loaded = saved_settings_.profiles_by_app;
  settings.clear_safe_mode = clear_safe_mode_;
  settings.safe_mode_enabled = saved_settings_.safe_mode_enabled;
  settings.safe_mode_entered_at = saved_settings_.safe_mode_entered_at;
  settings.safe_mode_last_crash_count = saved_settings_.safe_mode_last_crash_count;
  azookey::settings::SettingValues edited;
  if (std::string invalid_path; !field_controls_.Read(&edited, &invalid_path)) {
    ShowInvalidSetting(invalid_path);
    co_return;
  }
  // Only changed values are written, so untouched keys keep whatever is on disk.
  settings.values = azookey::settings::ChangedSettingValues(edited, saved_settings_.values);

  if (!settings.model_selected_path.empty()) {
    const std::filesystem::path model_path(ModelPathTextBox().Text().c_str());
    auto extension = model_path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    std::error_code ec;
    if (!model_path.is_absolute() || extension != L".gguf" ||
        !std::filesystem::is_regular_file(model_path, ec)) {
      Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
      ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error,
                 resources.GetString(L"SaveFailedTitle"), resources.GetString(L"InvalidModelPath"));
      co_return;
    }
  }

  SaveButton().IsEnabled(false);
  OpenAiApiKeyPasswordBox().IsEnabled(false);
  ClearOpenAiApiKeyButton().IsEnabled(false);
  SaveProgressRing().IsActive(true);
  SaveProgressRing().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
  SettingsStatusInfoBar().IsOpen(false);
  const auto path = *settings_path_;
  const auto dispatcher = DispatcherQueue();
  co_await winrt::resume_background();
  const auto save_result = azookey::settings::SaveSettingsDocument(path, settings);
  azookey::settings::SettingsIpcResult ipc_result;
  if (save_result.ok) {
    azookey::core::CrashReporting::SetConsent(settings.crash_report_consent == "local"
        ? azookey::core::CrashConsent::Local : azookey::core::CrashConsent::Off);
    ipc_result = azookey::settings::NotifyHostOfSettingsChange(
        azookey::settings::DefaultSettingsIpcOptions());
  }
  co_await ResumeForeground(dispatcher);

  SaveButton().IsEnabled(true);
  OpenAiApiKeyPasswordBox().IsEnabled(true);
  ClearOpenAiApiKeyButton().IsEnabled(true);
  UpdateCrashReportStatus();
  SaveProgressRing().IsActive(false);
  SaveProgressRing().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
  // The disk save is authoritative, even if notifying the Host failed.
  if (save_result.ok && settings.model_backend_preference) {
    UnsupportedBackendText().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
  }
  if (save_result.ok) {
    openai_api_key_changed_ = false;
    RefreshNeologdLayerStatus();
  }
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader final_resources;
  if (save_result.ok) {
    // sideload-packaging-spec section 3.6: UpdateConfig cannot apply these, so say when they will.
    if (!azookey::settings::SettingsRequiringHostRestart(saved_settings_, settings).empty()) {
      // An unpublished pack is never fetched, so a restart would change nothing.
      const bool published = neologd_attribution_ && neologd_attribution_->published;
      RestartRequiredInfoBar().Title(final_resources.GetString(
          published ? L"RestartRequiredTitle" : L"NeologdPackNotFetchedTitle"));
      RestartRequiredInfoBar().Message(final_resources.GetString(
          published ? L"NeologdRestartRequiredMessage" : L"NeologdPackUnpublished"));
      RestartRequiredInfoBar().IsOpen(true);
    } else if (!settings.dictionary->neologd_enabled) {
      // Turned back off before a restart, so the pack will not be fetched after all.
      RestartRequiredInfoBar().IsOpen(false);
    }
    if (save_result.safe_mode_reentered) {
      ShowSafeMode(saved_settings_);
      SafeModeInfoBar().Message(final_resources.GetString(L"SafeModeReentered"));
    } else if (clear_safe_mode_) {
      clear_safe_mode_ = false;
      settings.safe_mode_enabled = false;
      SafeModeInfoBar().IsOpen(false);
    }
    auto saved_values = std::move(saved_settings_.values);
    for (const auto& [setting, value] : settings.values) saved_values[setting] = value;
    saved_settings_ = settings;
    saved_settings_.values = std::move(saved_values);
    saved_settings_.openai_api_key.clear();
  }
  if (save_result.invalid_setting) {
    ShowInvalidSetting(*save_result.invalid_setting);
  } else if (!save_result.ok) {
    ShowStatus(
        Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error,
        final_resources.GetString(L"SaveFailedTitle"),
        final_resources.GetString(save_result.quarantined_path ? L"SaveFailedAfterQuarantineMessage"
                                                               : L"SaveFailedMessage"));
  } else if (!ipc_result.ok || !save_result.warnings.empty()) {
    winrt::hstring message;
    if (save_result.quarantined_path) {
      AppendMessage(&message, final_resources.GetString(L"InvalidFileQuarantinedMessage"));
    } else if (!save_result.warnings.empty()) {
      AppendMessage(&message, final_resources.GetString(L"InvalidEntriesRemovedMessage"));
    }
    if (!ipc_result.ok) {
      AppendMessage(&message, final_resources.GetString(L"HostUpdateFailedMessage"));
    }
    ShowStatus(
        Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning,
        final_resources.GetString(!ipc_result.ok ? L"SavedHostWarningTitle" : L"SaveWarningTitle"),
        message);
  } else {
    ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success,
               final_resources.GetString(L"SaveSucceededTitle"),
               final_resources.GetString(L"SaveSucceededMessage"));
  }
}

void MainWindow::ShowSafeMode(const azookey::settings::EditableSettings& settings) {
  clear_safe_mode_ = false;
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  ClearSafeModeButton().Content(winrt::box_value(resources.GetString(L"ClearSafeModeLabel")));
  SafeModeInfoBar().IsOpen(settings.safe_mode_enabled);
  if (!settings.safe_mode_enabled) return;
  auto message = resources.GetString(L"SafeModeMessage");
  if (!settings.safe_mode_entered_at.empty()) {
    message = message + L"\n" + resources.GetString(L"SafeModeEnteredAtLabel") +
              winrt::to_hstring(settings.safe_mode_entered_at);
  }
  message = message + L"\n" + resources.GetString(L"SafeModeCrashCountLabel") +
            winrt::to_hstring(settings.safe_mode_last_crash_count);
  SafeModeInfoBar().Message(message);
}

void MainWindow::ClearSafeModeButton_Click(Windows::Foundation::IInspectable const&,
                                           Microsoft::UI::Xaml::RoutedEventArgs const&) {
  // Only the user leaves SafeMode, and only through a save (sideload-packaging-spec section 3.6).
  // A second press withdraws the request before it is saved.
  if (clear_safe_mode_) {
    ShowSafeMode(saved_settings_);
    return;
  }
  clear_safe_mode_ = true;
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  ClearSafeModeButton().Content(winrt::box_value(resources.GetString(L"CancelClearSafeModeLabel")));
  SafeModeInfoBar().Message(resources.GetString(L"SafeModeClearPending"));
}

void MainWindow::ShowInvalidSetting(const std::string& path) {
  // Open the pane holding the field and put the focus on it, so the value can be corrected.
  if (path == "profilesByApp") SettingsNavigationView().SelectedItem(ProfilesNavigationItem());
  if (const auto* field = azookey::settings::FindSettingField(path)) {
    using Pane = azookey::settings::SettingsPane;
    const auto item = field->pane == Pane::General      ? GeneralNavigationItem()
                      : field->pane == Pane::Input      ? InputNavigationItem()
                      : field->pane == Pane::Dictionary ? DictionaryNavigationItem()
                      : field->pane == Pane::Ai         ? AiNavigationItem()
                      : field->pane == Pane::Privacy    ? PrivacyNavigationItem()
                                                        : AdvancedNavigationItem();
    SettingsNavigationView().SelectedItem(item);
    DispatcherQueue().TryEnqueue(
        [this, lifetime = get_strong(), path] { field_controls_.Focus(path); });
  }
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  ShowStatus(
      Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error,
      resources.GetString(L"SaveFailedTitle"),
      resources.GetString(L"InvalidSettingValue") +
          resources.GetString(winrt::to_hstring(azookey::settings::SettingLabelResource(path))));
}

void MainWindow::UpdateCrashReportStatus() {
  using azookey::core::CrashStatus;
  const auto status = azookey::core::CrashReporting::Status();
  const auto key = status == CrashStatus::Ready ? L"CrashReportReady" :
      status == CrashStatus::Disabled ? L"CrashReportDisabled" : L"CrashReportUnavailable";
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  CrashReportStatusText().Text(resources.GetString(key));
}

void MainWindow::OpenCrashReportsButton_Click(Windows::Foundation::IInspectable const&,
                                             Microsoft::UI::Xaml::RoutedEventArgs const&) {
  const auto directory = azookey::core::CrashReporting::DefaultDirectory();
  std::error_code error;
  if (!directory.empty() && std::filesystem::is_directory(directory, error)) {
    const auto opened = ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr,
                                      SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) > 32) return;
  }
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational,
              resources.GetString(L"CrashReportFolderTitle"),
              resources.GetString(L"CrashReportFolderUnavailable"));
}

void MainWindow::OpenThirdPartyNoticesButton_Click(Windows::Foundation::IInspectable const&,
                                                   Microsoft::UI::Xaml::RoutedEventArgs const&) {
  std::array<wchar_t, 32768> module_path{};
  const auto length =
      GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
  if (length > 0 && length < module_path.size()) {
    const auto notices =
        std::filesystem::path(std::wstring(module_path.data(), length)).parent_path() /
        L"ThirdPartyNotices.txt";
    std::error_code error;
    if (std::filesystem::is_regular_file(notices, error)) {
      const auto opened =
          ShellExecuteW(nullptr, L"open", notices.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      if (reinterpret_cast<INT_PTR>(opened) > 32) return;
    }
  }
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational,
             resources.GetString(L"ThirdPartyNoticesTitle"),
             resources.GetString(L"ThirdPartyNoticesUnavailable"));
}

void MainWindow::ApplyLaunchArguments(std::wstring_view raw_arguments) {
  const auto arguments = azookey::settings::ParseLaunchArguments(raw_arguments);
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;

  auto langid_status = arguments.langid_status;
  auto profile_status = arguments.profile_status;
  if (arguments.parse_error) {
    if (langid_status == azookey::settings::LaunchArgumentStatus::NotSupplied) {
      langid_status = azookey::settings::LaunchArgumentStatus::Invalid;
    }
    if (profile_status == azookey::settings::LaunchArgumentStatus::NotSupplied) {
      profile_status = azookey::settings::LaunchArgumentStatus::Invalid;
    }
  }

  if (langid_status == azookey::settings::LaunchArgumentStatus::Valid) {
    wchar_t langid_text[7]{};
    swprintf_s(langid_text, L"0x%04X", static_cast<unsigned int>(arguments.langid));
    LanguageValueText().Text(langid_text);
  } else {
    LanguageValueText().Text(StatusText(langid_status, resources));
  }

  if (profile_status == azookey::settings::LaunchArgumentStatus::Valid) {
    std::array<wchar_t, 40> profile_text{};
    if (StringFromGUID2(arguments.profile, profile_text.data(),
                        static_cast<int>(profile_text.size())) != 0) {
      ProfileValueText().Text(profile_text.data());
    } else {
      ProfileValueText().Text(resources.GetString(L"InvalidValue"));
    }
  } else {
    ProfileValueText().Text(StatusText(profile_status, resources));
  }
}

}  // namespace winrt::azookey_settings::implementation
