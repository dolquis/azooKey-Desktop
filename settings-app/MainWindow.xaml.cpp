// C++/WinRT requires the precompiled header before generated XAML headers.
// clang-format off
#include "pch.h"
#include "MainWindow.xaml.h"
#include "LaunchArguments.h"
#include "SettingsDocument.h"
#include "SettingsIpcClient.h"
#include "StatusPaneModel.h"
#include "UpdateInstaller.h"
#include "UpdateStatusModel.h"
#include "UiDispatch.h"
#include "ModelPaneModel.h"
// clang-format on

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"

#pragma comment(lib, "version.lib")
#endif

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <coroutine>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

#include "azookey/core/CrashReporting.h"
#include "azookey/core/PlatformPaths.h"

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

size_t ApiKeyHash(const winrt::hstring& key) {
  return std::hash<std::wstring_view>{}(std::wstring_view(key));
}

// Replaces the first "{0}" in a resource string.
winrt::hstring WithCount(const winrt::hstring& format, size_t count) {
  std::wstring text(format);
  if (const auto at = text.find(L"{0}"); at != std::wstring::npos) {
    text.replace(at, 3, std::to_wstring(count));
  }
  return winrt::hstring(text);
}

}  // namespace

namespace {

// This executable's product version as MAJOR.MINOR.PATCH (the VERSIONINFO the build writes from
// AZOOKEY_PRODUCT_VERSION, the value the Host records). Empty when it cannot be read.
std::string ProductVersion() {
  std::array<wchar_t, MAX_PATH> path{};
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return {};
  DWORD ignored = 0;
  const DWORD size = GetFileVersionInfoSizeW(path.data(), &ignored);
  if (size == 0) return {};
  std::vector<BYTE> data(size);
  if (!GetFileVersionInfoW(path.data(), 0, size, data.data())) return {};
  VS_FIXEDFILEINFO* info = nullptr;
  UINT info_size = 0;
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) || !info ||
      info_size < sizeof(VS_FIXEDFILEINFO)) {
    return {};
  }
  return std::to_string(HIWORD(info->dwProductVersionMS)) + "." +
         std::to_string(LOWORD(info->dwProductVersionMS)) + "." +
         std::to_string(HIWORD(info->dwProductVersionLS));
}

}  // namespace

namespace winrt::azookey_settings::implementation {

MainWindow::MainWindow() {
  InitializeComponent();
  SaveButton().IsEnabled(false);
  {
    Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
    const auto version = ProductVersion();
    std::wstring text(
        resources.GetString(version.empty() ? L"ProductVersion_Unknown" : L"ProductVersion_Text"));
    if (const auto at = text.find(L"{0}"); at != std::wstring::npos) {
      text.replace(at, 3, winrt::to_hstring(version));
    }
    ProductVersionText().Text(text);
  }
  field_controls_.Build([this](azookey::settings::SettingsPane pane) {
    using Pane = azookey::settings::SettingsPane;
    switch (pane) {
      case Pane::Input:
        return InputFieldsPanel();
      case Pane::Candidates:
        return CandidatesFieldsPanel();
      case Pane::Dictionary:
        return DictionaryFieldsPanel();
      case Pane::AiConversion:
        return AiConversionFieldsPanel();
      case Pane::AiService:
        return AiFieldsPanel();
      case Pane::Privacy:
        return PrivacyFieldsPanel();
      case Pane::Version:
        return VersionFieldsPanel();
      case Pane::Advanced:
        break;
    }
    return AdvancedFieldsPanel();
  });
  field_controls_.SetChangedHandler([this] {
    RefreshOpenAiKeyState();
    RefreshSaveBar();
  });
  // The panes keep these callbacks for requests that outlive a click, so they must not touch the
  // window once it has closed.
  Closed([alive = alive_](auto const&, auto const&) { *alive = false; });
  AppWindow().Closing(
      [this](auto const&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
        // DEV-1974: an install keeps the window open until msiexec ends, so the verified file
        // stays locked, the Host is brought back and a restart can be asked for.
        if (update_installing_) {
          args.Cancel(true);
          return;
        }
        if (close_confirmed_ || !controls_loaded_) return;
        // A save still running keeps the window open; its result decides what is left unsaved.
        if (saving_ || close_dialog_open_) {
          args.Cancel(true);
          return;
        }
        if (ChangedSettingCount() == 0) return;
        args.Cancel(true);
        ConfirmCloseAsync();
      });
  // The dedicated controls recount the save bar's changes as they are edited.
  const auto recount = [this](auto const&, auto const&) { RefreshSaveBar(); };
  ModelEnabledToggle().Toggled(recount);
  BackendPreferenceCombo().SelectionChanged(recount);
  LogLevelCombo().SelectionChanged(recount);
  CrashReportConsentToggle().Toggled(recount);
  SudachiDictionaryToggle().Toggled(recount);
  NamedEntityDictionaryToggle().Toggled(recount);
  TechnicalTermsDictionaryToggle().Toggled(recount);
  UserDictionaryToggle().Toggled(recount);
  AutoWordsDictionaryToggle().Toggled(recount);
  AppSpecificDictionaryToggle().Toggled(recount);
  profiles_pane_ = std::make_shared<azookey::settings::ProfilesPane>();
  profiles_pane_->Build(ProfilesPaneHost(), {[this, alive = alive_] {
                                               if (!*alive) throw winrt::hresult_error(E_ABORT);
                                               return Content().XamlRoot();
                                             },
                                             [this, alive = alive_] {
                                               if (*alive) RefreshSaveBar();
                                             }});
  model_pane_ = std::make_shared<azookey::settings::ModelPane>();
  model_pane_->Build(
      ModelsPaneHost(), DispatcherQueue(),
      {[this, alive = alive_] { return *alive ? model_selected_path_ : std::string(); },
       [this, alive = alive_](const std::string& path) {
         if (!*alive) return;
         model_selected_path_ = path;
         RefreshSaveBar();
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
                         },
                         [this] {
                           if (persona_pane_) persona_pane_->OnShown();
                         }});
  // "入力の傾向" is a tab of "学習" (sideload-packaging-spec section 3.2).
  persona_pane_ = std::make_shared<azookey::settings::PersonaPane>();
  persona_pane_->Build(learning_pane_->PersonaSection(), DispatcherQueue());
  proofread_pane_ = std::make_shared<azookey::settings::ProofreadPane>();
  proofread_pane_->Build(ProofreadPaneHost(), DispatcherQueue());
  SettingsNavigationView().SelectedItem(InputNavigationItem());
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
  InputPane().Visibility(visible(L"input"));
  CandidatesPane().Visibility(visible(L"candidates"));
  DictionaryPane().Visibility(visible(L"dictionary"));
  AiConversionPane().Visibility(visible(L"ai-conversion"));
  AiPane().Visibility(visible(L"ai-service"));
  PrivacyPane().Visibility(visible(L"privacy"));
  ProfilesPaneHost().Visibility(visible(L"profiles"));
  LearningPaneHost().Visibility(visible(L"learning"));
  ProofreadPaneHost().Visibility(visible(L"proofread"));
  AdvancedPane().Visibility(visible(L"advanced"));
  VersionPane().Visibility(visible(L"version"));
  SaveBar().Visibility(azookey::settings::PaneShowsSaveBar(tag)
                           ? Microsoft::UI::Xaml::Visibility::Visible
                           : Microsoft::UI::Xaml::Visibility::Collapsed);
  if (tag == L"ai-conversion" && model_pane_) model_pane_->OnShown();
  if (tag == L"learning" && learning_pane_) learning_pane_->OnShown();
  if (tag == L"dictionary") RefreshNeologdLayerStatus();
  if (tag == L"version") RefreshUpdateStatus();
}

void MainWindow::RefreshUpdateStatus() {
  // DEV-2076 (sideload-packaging-spec section 6.1): the Microsoft Store updates a package it
  // delivered, and the Host neither checks nor downloads under it. The pane then offers none of
  // the update controls and says why. A process's package identity does not change.
  static const bool managed_by_store = azookey::settings::UpdatesManagedByStore();
  VersionFieldsPanel().Visibility(managed_by_store ? Microsoft::UI::Xaml::Visibility::Collapsed
                                                   : Microsoft::UI::Xaml::Visibility::Visible);
  if (managed_by_store) {
    Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
    UpdateStatusText().Text(resources.GetString(L"UpdateStatus_StoreManaged"));
    OpenReleasePageButton().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
    UpdateInstallPanel().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
    return;
  }
  const auto* enabled_field = azookey::settings::FindSettingField("autoUpdate.enabled");
  const bool enabled = enabled_field && std::get<bool>(azookey::settings::SettingValueOrDefault(
                                            *enabled_field, saved_settings_.values));
  const auto* channel_field = azookey::settings::FindSettingField("autoUpdate.channel");
  const std::string channel = channel_field
                                  ? std::get<std::string>(azookey::settings::SettingValueOrDefault(
                                        *channel_field, saved_settings_.values))
                                  : std::string("stable");
  // The Host does not check under privacy.mode offline or secure.
  const auto* mode_field = azookey::settings::FindSettingField("privacy.mode");
  const auto mode = mode_field ? std::get<std::string>(azookey::settings::SettingValueOrDefault(
                                     *mode_field, saved_settings_.values))
                               : std::string("normal");
  const bool offline = mode == "offline" || mode == "secure";
  const auto status = settings_path_ ? azookey::settings::ReadUpdateStatus(
                                           settings_path_->parent_path() / L"update-status.json")
                                     : azookey::settings::UpdateStatusView{};
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  const auto resource =
      azookey::settings::UpdateStatusResource(status, enabled, offline, channel, ProductVersion());
  std::wstring text(resources.GetString(winrt::to_hstring(resource)));
  const auto replace = [&text](std::wstring_view name, std::wstring_view value) {
    if (const auto at = text.find(name); at != std::wstring::npos) {
      text.replace(at, name.size(), value);
    }
  };
  replace(L"{latest}", winrt::to_hstring(status.latest_version));
  std::wstring checked;
  if (status.checked_at_epoch) {
    // Local time of the check, as "2026-11-11 10:30".
    const uint64_t ticks = (*status.checked_at_epoch + 11644473600ULL) * 10000000ULL;
    FILETIME utc{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME utc_time{};
    SYSTEMTIME local{};
    if (FileTimeToSystemTime(&utc, &utc_time) &&
        SystemTimeToTzSpecificLocalTime(nullptr, &utc_time, &local)) {
      std::array<wchar_t, 32> buffer{};
      swprintf_s(buffer.data(), buffer.size(), L"%04u-%02u-%02u %02u:%02u",
                 static_cast<unsigned>(local.wYear), static_cast<unsigned>(local.wMonth),
                 static_cast<unsigned>(local.wDay), static_cast<unsigned>(local.wHour),
                 static_cast<unsigned>(local.wMinute));
      checked = buffer.data();
    }
  }
  replace(L"{checked}", checked.empty() ? std::wstring(L"-") : checked);
  UpdateStatusText().Text(text);

  // Only a release page of the repository is offered (UpdateStatusModel validates the URL).
  const bool link = resource == "UpdateStatus_UpdateAvailable" && !status.release_url.empty();
  if (link) {
    OpenReleasePageButton().NavigateUri(
        Windows::Foundation::Uri(winrt::to_hstring(status.release_url)));
  }
  OpenReleasePageButton().Visibility(link ? Microsoft::UI::Xaml::Visibility::Visible
                                          : Microsoft::UI::Xaml::Visibility::Collapsed);

  // DEV-1974: only an available update named as release.yml names its MSI is offered. A download
  // or an install in progress keeps the version it began with.
  std::string offered;
  if (resource == "UpdateStatus_UpdateAvailable" && settings_path_ &&
      azookey::settings::UpdatePackagePath(*settings_path_, status.latest_version)) {
    offered = status.latest_version;
  }
  if (!update_downloading_ && !update_installing_ && offered != update_version_) {
    update_version_ = offered;
    update_sha256_.clear();
    update_addon_sha256_.clear();
    update_installed_ = false;
    UpdateInstallText().Text(L"");
  }
  RenderUpdateInstall();
}

void MainWindow::RenderUpdateInstall() {
  using Microsoft::UI::Xaml::Visibility;
  const auto shown = [](bool visible) {
    return visible ? Visibility::Visible : Visibility::Collapsed;
  };
  const bool ready = !update_sha256_.empty();
  UpdateInstallPanel().Visibility(shown(!update_version_.empty()));
  DownloadUpdateButton().Visibility(shown(!update_installed_ && !update_downloading_ && !ready));
  DownloadUpdateButton().IsEnabled(!update_installing_);
  CancelUpdateDownloadButton().Visibility(shown(update_downloading_));
  InstallUpdateButton().Visibility(shown(!update_installed_ && !update_downloading_ && ready));
  InstallUpdateButton().IsEnabled(!update_installing_);
  UpdateDownloadProgress().Visibility(shown(update_downloading_));
}

winrt::fire_and_forget MainWindow::DownloadUpdateButton_Click(
    Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
  const auto lifetime = get_strong();
  if (update_downloading_ || update_installing_ || update_version_.empty()) co_return;
  const std::string version = update_version_;
  update_downloading_ = true;
  update_sha256_.clear();
  update_addon_sha256_.clear();
  UpdateInstallText().Text(L"");
  UpdateDownloadProgress().Value(0);
  RenderUpdateInstall();
  const auto dispatcher = DispatcherQueue();
  std::string resource = "UpdateInstall_Error_Other";
  std::string verified_sha;
  std::string verified_addon_sha;
  try {
    co_await winrt::resume_background();
    const auto options = azookey::settings::DefaultSettingsIpcOptions();
    azookey::ipc::FetchUpdateRequest request;
    request.version = version;
    const auto started = azookey::settings::RequestFetchUpdate(options, request);
    auto call_status = started.status;
    std::optional<azookey::ipc::UpdateFetchResponse> last = started.response;
    // A download of the same version another window started is followed as if this one had.
    const bool follow = call_status == azookey::settings::HostCallStatus::Ok && last &&
                        (last->ok || (last->error == "busy" && last->version == version));
    // A poll that gets no answer is retried; twenty in a row (at least ten seconds) end the wait.
    int unanswered = 0;
    while (follow && last->state == "running" && last->version == version) {
      co_await azookey::settings::ResumeForeground(dispatcher);
      if (last->total_bytes != 0) {
        UpdateDownloadProgress().Value(100.0 * static_cast<double>(last->received_bytes) /
                                       static_cast<double>(last->total_bytes));
        Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
        std::wstring text(resources.GetString(L"UpdateInstall_Downloading"));
        std::array<wchar_t, 32> received{};
        std::array<wchar_t, 32> total{};
        swprintf_s(received.data(), received.size(), L"%.1f",
                   static_cast<double>(last->received_bytes) / (1024.0 * 1024.0));
        swprintf_s(total.data(), total.size(), L"%.1f",
                   static_cast<double>(last->total_bytes) / (1024.0 * 1024.0));
        for (const auto& [name, value] :
             {std::pair<std::wstring_view, const wchar_t*>{L"{received}", received.data()},
              std::pair<std::wstring_view, const wchar_t*>{L"{total}", total.data()}}) {
          if (const auto at = text.find(name); at != std::wstring::npos) {
            text.replace(at, name.size(), value);
          }
        }
        UpdateInstallText().Text(text);
      }
      co_await winrt::resume_after(std::chrono::milliseconds(500));
      auto poll = azookey::settings::RequestQueryUpdateFetch(options);
      if (poll.status == azookey::settings::HostCallStatus::Ok && poll.response) {
        last = std::move(poll.response);
        unanswered = 0;
      } else if (++unanswered >= 20) {
        // An answer that could not be read is a broken response, not a Host that went away.
        call_status = poll.status == azookey::settings::HostCallStatus::Ok
                          ? azookey::settings::HostCallStatus::InvalidResponse
                          : poll.status;
        break;
      }
    }
    if (call_status != azookey::settings::HostCallStatus::Ok || !last) {
      resource = azookey::settings::HostCallStatusResource(call_status);
    } else if (!follow) {
      resource = azookey::settings::UpdateFetchErrorResource(last->error.value_or(""));
    } else if (last->state == "ready" && last->version == version &&
               azookey::settings::IsSha256Hex(last->sha256)) {
      verified_sha = last->sha256;
      // DEV-2081: the add-on of the same version, when the Host verified it too.
      if (azookey::settings::IsSha256Hex(last->addon_sha256)) {
        verified_addon_sha = last->addon_sha256;
      }
      resource = "UpdateInstall_Ready";
    } else if (last->state == "failed") {
      resource = azookey::settings::UpdateFetchErrorResource(last->result_error);
    }
  } catch (...) {
  }
  // The window may have closed meanwhile: then the dispatcher refuses the resume, or the
  // controls are gone, and nothing is left to update.
  try {
    co_await azookey::settings::ResumeForeground(dispatcher);
    if (!*alive_) co_return;
    update_downloading_ = false;
    // Kept only while the pane still offers that version.
    if (version == update_version_) {
      update_sha256_ = verified_sha;
      update_addon_sha256_ = verified_addon_sha;
    }
    Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
    std::wstring text(resources.GetString(winrt::to_hstring(resource)));
    if (const auto at = text.find(L"{version}"); at != std::wstring::npos) {
      text.replace(at, 9, winrt::to_hstring(version));
    }
    UpdateInstallText().Text(text);
    RenderUpdateInstall();
  } catch (...) {
  }
}

winrt::fire_and_forget MainWindow::CancelUpdateDownloadButton_Click(
    Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
  const auto lifetime = get_strong();
  if (!update_downloading_) co_return;
  const std::string version = update_version_;
  CancelUpdateDownloadButton().IsEnabled(false);
  const auto dispatcher = DispatcherQueue();
  try {
    co_await winrt::resume_background();
    azookey::ipc::CancelUpdateFetchRequest request;
    request.version = version;
    // The download then ends as failed / "cancelled", which the running poll reports.
    (void)azookey::settings::RequestCancelUpdateFetch(
        azookey::settings::DefaultSettingsIpcOptions(), request);
  } catch (...) {
  }
  try {
    co_await azookey::settings::ResumeForeground(dispatcher);
    if (*alive_) CancelUpdateDownloadButton().IsEnabled(true);
  } catch (...) {
    // The window closed meanwhile.
  }
}

winrt::fire_and_forget MainWindow::InstallUpdateButton_Click(
    Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
  const auto lifetime = get_strong();
  if (update_installing_ || update_downloading_ || update_sha256_.empty() || !settings_path_) {
    co_return;
  }
  const std::string version = update_version_;
  const std::string sha256 = update_sha256_;
  const auto package_path = azookey::settings::UpdatePackagePath(*settings_path_, version);
  if (!package_path) co_return;
  // DEV-2081: an installed add-on follows the base when the Host verified its MSI of the version.
  const bool addon_installed = azookey::settings::VulkanAddonInstalled();
  const std::string addon_sha256 = update_addon_sha256_;
  const auto addon_path = azookey::settings::UpdateAddonPackagePath(*settings_path_, version);
  const bool with_addon = addon_installed && !addon_sha256.empty() && addon_path.has_value();

  // Every text the result can need is read now: the install replaces this app's own files.
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  const auto text = [&resources](const wchar_t* name) {
    return std::wstring(resources.GetString(name));
  };
  const auto verify_failed = text(L"UpdateInstall_VerifyFailed");
  const auto installing = text(L"UpdateInstall_Installing");
  const auto installed = text(L"UpdateInstall_Installed");
  const auto reboot_pending = text(L"UpdateInstall_RebootPending");
  const auto cancelled = text(L"UpdateInstall_Cancelled");
  const auto busy = text(L"UpdateInstall_Busy");
  const auto failed = text(L"UpdateInstall_Failed");
  const auto host_start_failed = text(L"UpdateInstall_HostStartFailed");
  const auto reboot_title = text(L"UpdateInstall_RebootTitle");
  const auto reboot_body = text(L"UpdateInstall_RebootBody");
  const auto reboot_primary = text(L"UpdateInstall_RebootPrimary");
  const auto reboot_close = text(L"UpdateInstall_RebootClose");
  const auto restart_failed = text(L"UpdateInstall_RestartFailed");
  const auto addon_installed_text = text(L"UpdateInstall_AddonInstalled");
  const auto addon_failed = text(L"UpdateInstall_AddonFailed");
  const auto addon_failed_no_code = text(L"UpdateInstall_AddonFailedNoCode");
  const auto addon_cancelled = text(L"UpdateInstall_AddonCancelled");
  const auto addon_verify_failed = text(L"UpdateInstall_AddonVerifyFailed");

  std::wstring body = text(L"UpdateInstall_ConfirmBody");
  if (with_addon) {
    // Two elevations: the base, then the add-on (sideload-packaging-spec section 6.3).
    body += L"\n\n" + text(L"UpdateInstall_ConfirmVulkanFollows");
  } else if (addon_installed) {
    // The add-on stays at the old version, which its Host refuses (msix-packaging-spec 1.6.3).
    body += L"\n\n" + text(L"UpdateInstall_ConfirmVulkan");
  }
  Microsoft::UI::Xaml::Controls::ContentDialog confirm;
  confirm.XamlRoot(Content().XamlRoot());
  confirm.Title(winrt::box_value(resources.GetString(L"UpdateInstall_ConfirmTitle")));
  Microsoft::UI::Xaml::Controls::TextBlock confirm_text;
  confirm_text.Text(body);
  confirm_text.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
  confirm.Content(confirm_text);
  confirm.PrimaryButtonText(resources.GetString(L"UpdateInstall_ConfirmPrimary"));
  confirm.CloseButtonText(resources.GetString(L"UpdateInstall_ConfirmClose"));
  confirm.DefaultButton(Microsoft::UI::Xaml::Controls::ContentDialogButton::Close);
  // Marked before the dialog, so a second press finds the install under way; a dialog that
  // cannot open (another is showing) is a declined install, not a crash of this window.
  update_installing_ = true;
  RenderUpdateInstall();
  bool accepted = false;
  try {
    accepted =
        co_await confirm.ShowAsync() == Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary;
  } catch (...) {
    accepted = false;
  }
  if (!accepted) {
    update_installing_ = false;
    RenderUpdateInstall();
    co_return;
  }

  UpdateInstallText().Text(installing);
  HWND owner = nullptr;
  if (const auto native = try_as<::IWindowNative>()) (void)native->get_WindowHandle(&owner);
  const auto dispatcher = DispatcherQueue();
  bool verified = false;
  bool host_started = true;
  azookey::settings::UpdateInstallResult result;
  bool addon_attempted = false;
  bool addon_verified = false;
  azookey::settings::UpdateInstallResult addon_result;
  try {
    co_await winrt::resume_background();
    // The handle denies writes and deletion from the verification until msiexec has ended.
    if (auto package = azookey::settings::VerifiedUpdatePackage::Open(*package_path, sha256)) {
      verified = true;
      result = azookey::settings::RunElevatedMsiexec(*package, owner);
    }
    // The add-on only after an applied base, which its MSI requires; its failure keeps the base.
    if (with_addon && verified && azookey::settings::AddonFollowsBase(result.outcome)) {
      addon_attempted = true;
      // Its own try: whatever happens to the add-on, the applied base still brings the Host back.
      try {
        if (auto addon =
                azookey::settings::VerifiedUpdatePackage::Open(*addon_path, addon_sha256)) {
          addon_verified = true;
          addon_result = azookey::settings::RunElevatedMsiexec(*addon, owner);
        }
      } catch (...) {
        addon_result = {};  // Failed.
      }
    }
    // Once both are done, so the supervisor finds the versions matched (msix-packaging-spec
    // 1.6.3). The installation stopped the Host; bring it back (sideload-packaging-spec 6.3).
    if (verified && (result.outcome == azookey::settings::UpdateInstallOutcome::Installed ||
                     result.outcome == azookey::settings::UpdateInstallOutcome::RebootRequired)) {
      host_started = azookey::settings::StartInstalledHost();
    }
  } catch (...) {
  }
  co_await azookey::settings::ResumeForeground(dispatcher);
  update_installing_ = false;
  if (!verified) {
    update_sha256_.clear();
    UpdateInstallText().Text(verify_failed);
    RenderUpdateInstall();
    co_return;
  }
  using azookey::settings::UpdateInstallOutcome;
  std::wstring message;
  switch (result.outcome) {
    case UpdateInstallOutcome::Installed:
      message = installed;
      break;
    case UpdateInstallOutcome::RebootRequired:
      message = reboot_pending;
      break;
    case UpdateInstallOutcome::Cancelled:
      message = cancelled;
      break;
    case UpdateInstallOutcome::Busy:
      message = busy;
      break;
    case UpdateInstallOutcome::Failed:
      message = failed;
      if (const auto at = message.find(L"{code}"); at != std::wstring::npos) {
        message.replace(at, 6, std::to_wstring(result.code));
      }
      break;
  }
  const bool applied = result.outcome == UpdateInstallOutcome::Installed ||
                       result.outcome == UpdateInstallOutcome::RebootRequired;
  if (applied) {
    update_installed_ = true;
    update_sha256_.clear();
    update_addon_sha256_.clear();
    // A base done at once with an add-on awaiting a restart still needs that restart.
    if (result.outcome == UpdateInstallOutcome::Installed && addon_verified &&
        addon_result.outcome == UpdateInstallOutcome::RebootRequired) {
      message = reboot_pending;
    }
    if (addon_attempted) {
      // The base stays updated whatever happened to the add-on; a failure only guides the user.
      if (!addon_verified) {
        message += L"\n" + addon_verify_failed;
      } else if (azookey::settings::AddonFollowsBase(addon_result.outcome)) {
        message += L"\n" + addon_installed_text;
      } else if (addon_result.outcome == UpdateInstallOutcome::Cancelled) {
        // The second UAC prompt declined, or 1602.
        message += L"\n" + addon_cancelled;
      } else if (addon_result.code == 0) {
        // An exception, with no msiexec exit code to show.
        message += L"\n" + addon_failed_no_code;
      } else {
        std::wstring addon_message = addon_failed;
        if (const auto at = addon_message.find(L"{code}"); at != std::wstring::npos) {
          addon_message.replace(at, 6, std::to_wstring(addon_result.code));
        }
        message += L"\n" + addon_message;
      }
    }
    if (!host_started) message += L"\n" + host_start_failed;
  }
  UpdateInstallText().Text(message);
  RenderUpdateInstall();
  // One restart dialog when either install asked for it.
  const bool reboot_required =
      result.outcome == UpdateInstallOutcome::RebootRequired ||
      (addon_verified && addon_result.outcome == UpdateInstallOutcome::RebootRequired);
  if (!reboot_required) co_return;

  // 3010: never restart on our own; ask first (section 6.3).
  Microsoft::UI::Xaml::Controls::ContentDialog reboot;
  reboot.XamlRoot(Content().XamlRoot());
  reboot.Title(winrt::box_value(winrt::hstring(reboot_title)));
  Microsoft::UI::Xaml::Controls::TextBlock reboot_text;
  reboot_text.Text(reboot_body);
  reboot_text.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
  reboot.Content(reboot_text);
  reboot.PrimaryButtonText(reboot_primary);
  reboot.CloseButtonText(reboot_close);
  reboot.DefaultButton(Microsoft::UI::Xaml::Controls::ContentDialogButton::Close);
  bool restart = false;
  try {
    restart =
        co_await reboot.ShowAsync() == Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary;
  } catch (...) {
    // Another dialog was open: the pane's text still says a restart finishes the update.
    restart = false;
  }
  if (restart && !azookey::settings::RequestRestart()) {
    UpdateInstallText().Text(message + L"\n" + restart_failed);
  }
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
    RefreshSaveBar();
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
  RefreshSaveBar();
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
    can_save_ = false;
    RefreshSaveBar();
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
  // Events raised while the controls take the loaded values are not counted as edits.
  controls_loaded_ = false;
  can_save_ = result.status != azookey::settings::SettingsDocumentStatus::LockUnavailable &&
              result.status != azookey::settings::SettingsDocumentStatus::ReadError;
  // The load message says a save rewrites the file without the invalid entries.
  repair_pending_ = azookey::settings::SaveRepairsLoadedDocument(result);
  saved_settings_ = result.settings;
  saved_settings_.openai_api_key.clear();  // Only compared for restart; keep no extra secret copy.
  ApplyDictionaryToControls(
      result.settings.dictionary.value_or(azookey::settings::DictionarySettings{}));
  field_controls_.Apply(result.settings.values);
  RefreshOpenAiKeyState();
  ShowSafeMode(result.settings);
  ModelEnabledToggle().IsOn(result.settings.model_enabled);
  model_selected_path_ = result.settings.model_selected_path;
  model_pane_->ShowSelection();
  model_pane_->SetHistory(result.settings.benchmark_history);
  // The stored switch decides how a not_requested layer reads, so ask again now it is known.
  if (DictionaryPane().Visibility() == Microsoft::UI::Xaml::Visibility::Visible) {
    RefreshNeologdLayerStatus();
  }
  profiles_pane_->SetProfiles(
      result.settings.profiles_by_app.value_or(azookey::settings::AppProfiles{}),
      result.settings.legacy_prompt_prefixes);
  OpenAiApiKeyPasswordBox().Password(winrt::to_hstring(result.settings.openai_api_key));
  loaded_api_key_hash_ = ApiKeyHash(OpenAiApiKeyPasswordBox().Password());
  openai_api_key_cleared_ = false;
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
    load_problem_shown_ = true;
  }
  controls_loaded_ = true;
  RefreshSaveBar();
}

void MainWindow::ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
                            const winrt::hstring& title, const winrt::hstring& message) {
  load_problem_shown_ = false;
  SettingsStatusInfoBar().Severity(severity);
  SettingsStatusInfoBar().Title(title);
  SettingsStatusInfoBar().Message(message);
  SettingsStatusInfoBar().IsOpen(true);
}

void MainWindow::ShowSaveStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
                                const winrt::hstring& title, const winrt::hstring& message) {
  SaveStatusInfoBar().Severity(severity);
  SaveStatusInfoBar().Title(title);
  SaveStatusInfoBar().Message(message);
  SaveStatusInfoBar().IsOpen(true);
  // A save asked for when closing may run from a pane without the save bar; show its result.
  SaveBar().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
}

azookey::settings::EditableSettings MainWindow::SettingsFromControls(
    std::vector<std::string>* invalid_paths) {
  azookey::settings::EditableSettings settings;
  settings.crash_report_consent = CrashReportConsentToggle().IsOn() ? "local" : "off";
  settings.model_enabled = ModelEnabledToggle().IsOn();
  const int backend_index = BackendPreferenceCombo().SelectedIndex();
  if (backend_index == 0 || backend_index == 1) {
    settings.model_backend_preference = backend_index == 1 ? "cpu" : "auto";
  } else {
    settings.model_backend_preference.reset();
  }
  settings.model_selected_path = model_selected_path_;
  // Compared by hash, so no copy of the key is kept, and the box's text is copied into a string
  // only when it is to be written. A clear is always written, which also replaces a stored key
  // that could not be decrypted.
  const auto api_key = OpenAiApiKeyPasswordBox().Password();
  settings.openai_api_key_changed =
      openai_api_key_cleared_ || ApiKeyHash(api_key) != loaded_api_key_hash_;
  if (settings.openai_api_key_changed) settings.openai_api_key = winrt::to_string(api_key);
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
  settings.values = field_controls_.ReadValid(invalid_paths);
  return settings;
}

size_t MainWindow::ChangedSettingCount() {
  std::vector<std::string> invalid;
  const auto settings = SettingsFromControls(&invalid);
  return azookey::settings::ChangedSettingPaths(settings, saved_settings_).size() + invalid.size();
}

void MainWindow::RefreshSaveBar() {
  if (!controls_loaded_) {
    SaveButton().IsEnabled(false);
    return;
  }
  const size_t count = ChangedSettingCount();
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  if (count > 0) {
    ChangeCountText().Text(WithCount(resources.GetString(L"SaveBar_ChangeCount"), count));
  } else {
    ChangeCountText().Text(
        resources.GetString(repair_pending_ ? L"SaveBar_RepairPending" : L"SaveBar_NoChanges"));
  }
  SaveButton().IsEnabled(
      azookey::settings::SaveBarAllowsSave(can_save_, saving_, count, repair_pending_));
}

winrt::fire_and_forget MainWindow::ConfirmCloseAsync() {
  const auto lifetime = get_strong();
  close_dialog_open_ = true;
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  Microsoft::UI::Xaml::Controls::ContentDialog dialog;
  dialog.XamlRoot(Content().XamlRoot());
  dialog.Title(winrt::box_value(resources.GetString(L"SaveBar_UnsavedTitle")));
  dialog.Content(winrt::box_value(
      WithCount(resources.GetString(L"SaveBar_UnsavedMessage"), ChangedSettingCount())));
  dialog.PrimaryButtonText(resources.GetString(L"SaveBar_UnsavedSave"));
  // A file that could not be read is never overwritten (the save button is off as well).
  dialog.IsPrimaryButtonEnabled(can_save_);
  dialog.SecondaryButtonText(resources.GetString(L"SaveBar_UnsavedDiscard"));
  dialog.CloseButtonText(resources.GetString(L"SaveBar_UnsavedCancel"));
  dialog.DefaultButton(Microsoft::UI::Xaml::Controls::ContentDialogButton::Primary);
  bool close = false;
  try {
    const auto choice = co_await dialog.ShowAsync();
    if (choice == Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary) {
      // A failed or refused save leaves the window open with the reason in the save bar.
      close = co_await SaveSettingsCoreAsync();
    } else {
      close = choice == Microsoft::UI::Xaml::Controls::ContentDialogResult::Secondary;
    }
  } catch (...) {
    // For example another dialog is already open; keep the window and its changes.
    close = false;
  }
  close_dialog_open_ = false;
  if (close && *alive_) {
    close_confirmed_ = true;
    Close();
  }
}

winrt::fire_and_forget MainWindow::SaveButton_Click(Windows::Foundation::IInspectable const&,
                                                    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    co_await SaveSettingsCoreAsync();
  } catch (...) {
    co_return;
  }
}

void MainWindow::RefreshOpenAiKeyState() {
  const auto& condition = azookey::settings::OpenAiCondition();
  const bool openai = field_controls_.ConditionHolds(condition);
  OpenAiApiKeyPasswordBox().IsEnabled(!saving_ && openai);
  ClearOpenAiApiKeyButton().IsEnabled(!saving_);
  Microsoft::UI::Xaml::Automation::AutomationProperties::SetHelpText(
      OpenAiApiKeyPasswordBox(),
      openai ? winrt::hstring{} : field_controls_.ConditionText(condition));
}

void MainWindow::OpenAiApiKeyPasswordBox_PasswordChanged(
    Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
  RefreshSaveBar();
}

void MainWindow::ClearOpenAiApiKeyButton_Click(Windows::Foundation::IInspectable const&,
                                               Microsoft::UI::Xaml::RoutedEventArgs const&) {
  OpenAiApiKeyPasswordBox().Password(L"");
  openai_api_key_cleared_ = true;
  RefreshSaveBar();
}

Windows::Foundation::IAsyncOperation<bool> MainWindow::SaveSettingsCoreAsync() {
  const auto lifetime = get_strong();
  if (saving_ || !can_save_) co_return false;
  if (!settings_path_) {
    Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
    ShowSaveStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error,
                   resources.GetString(L"SaveFailedTitle"),
                   resources.GetString(L"LocalAppDataUnavailable"));
    co_return false;
  }

  std::vector<std::string> invalid;
  auto settings = SettingsFromControls(&invalid);
  if (!invalid.empty()) {
    ShowInvalidSetting(invalid.front());
    co_return false;
  }
  // Only changed values are written, so untouched keys keep whatever is on disk.
  settings.values =
      azookey::settings::ChangedSettingValues(settings.values, saved_settings_.values);

  if (!settings.model_selected_path.empty()) {
    // Built from the wide string, so a path with Japanese in it is not read in the ANSI code page.
    const std::filesystem::path model_path(winrt::to_hstring(settings.model_selected_path).c_str());
    if (!azookey::settings::IsUsableModelFile(model_path)) {
      Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
      ShowSaveStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error,
                     resources.GetString(L"SaveFailedTitle"),
                     resources.GetString(L"InvalidModelPath"));
      co_return false;
    }
  }

  saving_ = true;
  RefreshSaveBar();
  RefreshOpenAiKeyState();
  SaveProgressRing().IsActive(true);
  SaveProgressRing().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
  SaveStatusInfoBar().IsOpen(false);
  const auto path = *settings_path_;
  const auto dispatcher = DispatcherQueue();
  co_await winrt::resume_background();
  // A throw here is reported as a failed save, so the save bar (and closing) is never left
  // waiting for a save that has ended.
  azookey::settings::SettingsSaveResult save_result;
  try {
    save_result = azookey::settings::SaveSettingsDocument(path, settings);
  } catch (...) {
    save_result = {};
  }
  azookey::settings::SettingsIpcResult ipc_result;
  if (save_result.ok) {
    try {
      azookey::core::CrashReporting::SetConsent(settings.crash_report_consent == "local"
                                                    ? azookey::core::CrashConsent::Local
                                                    : azookey::core::CrashConsent::Off);
      ipc_result = azookey::settings::NotifyHostOfSettingsChange(
          azookey::settings::DefaultSettingsIpcOptions());
    } catch (...) {
      ipc_result = {};
    }
  }
  co_await ResumeForeground(dispatcher);

  saving_ = false;
  RefreshOpenAiKeyState();
  UpdateCrashReportStatus();
  SaveProgressRing().IsActive(false);
  SaveProgressRing().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
  // The disk save is authoritative, even if notifying the Host failed.
  if (save_result.ok && settings.model_backend_preference) {
    UnsupportedBackendText().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
  }
  if (save_result.ok) {
    if (settings.openai_api_key_changed) {
      // From the box, as when loading: a UTF-8 round trip could change what the box holds.
      loaded_api_key_hash_ = ApiKeyHash(OpenAiApiKeyPasswordBox().Password());
    }
    openai_api_key_cleared_ = false;
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
    // The file no longer holds what loading reported once a save has repaired it.
    const bool repaired = repair_pending_;
    repair_pending_ = false;
    if (repaired && load_problem_shown_) {
      load_problem_shown_ = false;
      SettingsStatusInfoBar().IsOpen(false);
    }
    // The update status reads the saved autoUpdate and privacy.mode, which the same pane edits.
    RefreshUpdateStatus();
  }
  RefreshSaveBar();
  if (save_result.invalid_setting) {
    ShowInvalidSetting(*save_result.invalid_setting);
  } else if (!save_result.ok) {
    ShowSaveStatus(
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
    ShowSaveStatus(
        Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning,
        final_resources.GetString(!ipc_result.ok ? L"SavedHostWarningTitle" : L"SaveWarningTitle"),
        message);
  } else {
    ShowSaveStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success,
                   final_resources.GetString(L"SaveSucceededTitle"),
                   final_resources.GetString(L"SaveSucceededMessage"));
  }
  co_return save_result.ok;
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
    RefreshSaveBar();
    return;
  }
  clear_safe_mode_ = true;
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  ClearSafeModeButton().Content(winrt::box_value(resources.GetString(L"CancelClearSafeModeLabel")));
  SafeModeInfoBar().Message(resources.GetString(L"SafeModeClearPending"));
  RefreshSaveBar();
}

void MainWindow::ShowInvalidSetting(const std::string& path) {
  // Open the pane holding the field and put the focus on it, so the value can be corrected.
  if (path == "profilesByApp") SettingsNavigationView().SelectedItem(ProfilesNavigationItem());
  if (const auto* field = azookey::settings::FindSettingField(path)) {
    using Pane = azookey::settings::SettingsPane;
    const auto item = field->pane == Pane::Input          ? InputNavigationItem()
                      : field->pane == Pane::Candidates   ? CandidatesNavigationItem()
                      : field->pane == Pane::Dictionary   ? DictionaryNavigationItem()
                      : field->pane == Pane::AiConversion ? AiConversionNavigationItem()
                      : field->pane == Pane::AiService    ? AiNavigationItem()
                      : field->pane == Pane::Privacy      ? PrivacyNavigationItem()
                      : field->pane == Pane::Version      ? VersionNavigationItem()
                                                          : AdvancedNavigationItem();
    SettingsNavigationView().SelectedItem(item);
    DispatcherQueue().TryEnqueue(
        [this, lifetime = get_strong(), path] { field_controls_.Focus(path); });
  }
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  ShowSaveStatus(
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

void MainWindow::OpenLogFolderButton_Click(Windows::Foundation::IInspectable const&,
                                           Microsoft::UI::Xaml::RoutedEventArgs const&) {
  // The directory RuntimeLogger writes to; it exists once a log has been written.
  const auto local = azookey::core::GetLocalAppDataDirectory();
  if (local) {
    const auto directory = *local / L"azooKey" / L"logs";
    std::error_code error;
    if (std::filesystem::is_directory(directory, error)) {
      const auto opened =
          ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      if (reinterpret_cast<INT_PTR>(opened) > 32) return;
    }
  }
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational,
             resources.GetString(L"LogFolderTitle"), resources.GetString(L"LogFolderUnavailable"));
}

void MainWindow::OpenUserDictionaryButton_Click(Windows::Foundation::IInspectable const&,
                                                Microsoft::UI::Xaml::RoutedEventArgs const&) {
  // The tab first, so showing the pane lists the user dictionary rather than the first store.
  learning_pane_->OpenStoreTab("user_dict");
  SettingsNavigationView().SelectedItem(LearningNavigationItem());
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

void MainWindow::SelectPane(std::wstring_view tag) {
  const auto navigation = SettingsNavigationView();
  for (const auto& items : {navigation.MenuItems(), navigation.FooterMenuItems()}) {
    for (const auto& entry : items) {
      const auto item = entry.try_as<Microsoft::UI::Xaml::Controls::NavigationViewItem>();
      if (item && winrt::unbox_value_or<winrt::hstring>(item.Tag(), L"") == tag) {
        navigation.SelectedItem(item);
        return;
      }
    }
  }
}

void MainWindow::ApplyLaunchArguments(std::wstring_view raw_arguments) {
  const auto arguments = azookey::settings::ParseLaunchArguments(raw_arguments);
  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;

  // Each launch, including one forwarded to this window, decides whether the context is shown.
  LaunchContextInfoBar().Visibility(azookey::settings::HasLaunchContext(arguments)
                                        ? Microsoft::UI::Xaml::Visibility::Visible
                                        : Microsoft::UI::Xaml::Visibility::Collapsed);
  const auto langid_status = arguments.langid_status;
  const auto profile_status = arguments.profile_status;

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

  // Ctrl+Shift+Space in an application (rich-features-spec X-3-6) wins over --pane (section 3.5).
  if (arguments.proofread_session_status == azookey::settings::LaunchArgumentStatus::Valid) {
    SettingsNavigationView().SelectedItem(ProofreadNavigationItem());
    proofread_pane_->OpenSession(arguments.proofread_session_token);
  } else if (!arguments.pane.empty()) {
    SelectPane(arguments.pane);
  }
}

}  // namespace winrt::azookey_settings::implementation
