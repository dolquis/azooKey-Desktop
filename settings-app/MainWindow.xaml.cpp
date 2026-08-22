// C++/WinRT requires the precompiled header before generated XAML headers.
// clang-format off
#include "pch.h"
#include "MainWindow.xaml.h"
#include "LaunchArguments.h"
#include "SettingsDocument.h"
#include "SettingsIpcClient.h"
// clang-format on

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <algorithm>
#include <array>
#include <coroutine>
#include <cwctype>
#include <filesystem>
#include <string>

namespace {

struct DispatcherQueueAwaiter {
  winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher;

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> continuation) const {
    if (!dispatcher.TryEnqueue([continuation]() noexcept { continuation.resume(); })) {
      throw winrt::hresult_error(E_ABORT, L"The UI dispatcher is shutting down.");
    }
  }

  void await_resume() const noexcept {}
};

DispatcherQueueAwaiter ResumeForeground(
    winrt::Microsoft::UI::Dispatching::DispatcherQueue const& dispatcher) {
  return {dispatcher};
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
  settings_path_ = azookey::settings::DefaultSettingsPath();
  LoadSettingsAsync();
}

winrt::fire_and_forget MainWindow::LoadSettingsAsync() {
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
  SaveButton().IsEnabled(result.status !=
                             azookey::settings::SettingsDocumentStatus::LockUnavailable &&
                         result.status != azookey::settings::SettingsDocumentStatus::ReadError);
  ModelEnabledToggle().IsOn(result.settings.model_enabled);
  ModelPathTextBox().Text(winrt::to_hstring(result.settings.model_selected_path));
  if (result.settings.model_backend_preference) {
    const auto& backend = *result.settings.model_backend_preference;
    BackendPreferenceCombo().SelectedIndex(backend == "cpu" ? 1 : backend == "cuda" ? 2 : 0);
    UnsupportedBackendText().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
  } else {
    BackendPreferenceCombo().SelectedIndex(-1);
    Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
    UnsupportedBackendText().Text(resources.GetString(L"UnsupportedBackendPrefix") +
                                  winrt::to_hstring(result.settings.hidden_backend_preference));
    UnsupportedBackendText().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
  }

  const auto& log_level = result.settings.log_level;
  LogLevelCombo().SelectedIndex(log_level == "error"   ? 0
                                : log_level == "warn"  ? 1
                                : log_level == "debug" ? 3
                                                       : 2);

  if (result.error || !result.warnings.empty()) {
    Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
    const auto message = result.error ? winrt::to_hstring(*result.error)
                                      : resources.GetString(L"InvalidEntriesSkipped");
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
  const auto lifetime = get_strong();
  if (!settings_path_) {
    Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
    ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error,
               resources.GetString(L"SaveFailedTitle"),
               resources.GetString(L"LocalAppDataUnavailable"));
    co_return;
  }

  azookey::settings::EditableSettings settings;
  settings.model_enabled = ModelEnabledToggle().IsOn();
  const int backend_index = BackendPreferenceCombo().SelectedIndex();
  if (backend_index >= 0) {
    settings.model_backend_preference = backend_index == 1   ? "cpu"
                                        : backend_index == 2 ? "cuda"
                                                             : "auto";
  } else {
    settings.model_backend_preference.reset();
  }
  settings.model_selected_path = winrt::to_string(ModelPathTextBox().Text());
  const int log_index = LogLevelCombo().SelectedIndex();
  settings.log_level = log_index == 0   ? "error"
                       : log_index == 1 ? "warn"
                       : log_index == 3 ? "debug"
                                        : "info";

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
  SaveProgressRing().IsActive(true);
  SaveProgressRing().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
  SettingsStatusInfoBar().IsOpen(false);
  const auto path = *settings_path_;
  const auto dispatcher = DispatcherQueue();
  co_await winrt::resume_background();
  const auto save_result = azookey::settings::SaveSettingsDocument(path, settings);
  azookey::settings::SettingsIpcResult ipc_result;
  if (save_result.ok) {
    ipc_result = azookey::settings::NotifyHostOfSettingsChange(
        azookey::settings::DefaultSettingsIpcOptions());
  }
  co_await ResumeForeground(dispatcher);

  SaveButton().IsEnabled(true);
  SaveProgressRing().IsActive(false);
  SaveProgressRing().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader final_resources;
  if (!save_result.ok) {
    ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error,
               final_resources.GetString(L"SaveFailedTitle"),
               winrt::to_hstring(save_result.error.value_or("failed to save settings.json")));
  } else if (!ipc_result.ok) {
    ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning,
               final_resources.GetString(L"SavedHostWarningTitle"),
               winrt::to_hstring(ipc_result.error.value_or("UpdateConfig failed")));
  } else {
    ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success,
               final_resources.GetString(L"SaveSucceededTitle"),
               final_resources.GetString(L"SaveSucceededMessage"));
  }
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
