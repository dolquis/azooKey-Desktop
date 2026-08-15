// C++/WinRT requires the precompiled header before generated XAML headers.
// clang-format off
#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
// clang-format on

#include <microsoft.ui.xaml.window.h>

namespace winrt::azookey_settings::implementation {

App::App() { InitializeComponent(); }

App::~App() {
  if (app_instance_ != nullptr && activated_token_.value != 0) {
    app_instance_.Activated(activated_token_);
  }
}

void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) { LaunchAsync(); }

winrt::fire_and_forget App::LaunchAsync() {
  const auto lifetime = get_strong();
  const auto current_instance = Microsoft::Windows::AppLifecycle::AppInstance::GetCurrent();
  const auto activation_arguments = current_instance.GetActivatedEventArgs();
  const auto main_instance =
      Microsoft::Windows::AppLifecycle::AppInstance::FindOrRegisterForKey(L"main");
  if (!main_instance.IsCurrent()) {
    co_await main_instance.RedirectActivationToAsync(activation_arguments);
    Exit();
    co_return;
  }

  app_instance_ = main_instance;
  window_ = winrt::make<MainWindow>();
  dispatcher_queue_ = window_.DispatcherQueue();
  activated_token_ = app_instance_.Activated({this, &App::OnActivated});

  Microsoft::Windows::ApplicationModel::Resources::ResourceLoader resources;
  window_.Title(resources.GetString(L"WindowTitle"));
  ApplyActivation(activation_arguments);
  window_.Activate();
}

void App::OnActivated(Windows::Foundation::IInspectable const&,
                      Microsoft::Windows::AppLifecycle::AppActivationArguments const& arguments) {
  const auto lifetime = get_strong();
  dispatcher_queue_.TryEnqueue([this, lifetime, arguments] {
    ApplyActivation(arguments);
    RestoreAndActivateWindow();
  });
}

void App::ApplyActivation(
    Microsoft::Windows::AppLifecycle::AppActivationArguments const& arguments) {
  winrt::hstring raw_arguments;
  if (arguments.Kind() == Microsoft::Windows::AppLifecycle::ExtendedActivationKind::Launch) {
    if (const auto launch =
            arguments.Data()
                .try_as<Windows::ApplicationModel::Activation::ILaunchActivatedEventArgs>()) {
      raw_arguments = launch.Arguments();
    }
  } else if (arguments.Kind() ==
             Microsoft::Windows::AppLifecycle::ExtendedActivationKind::CommandLineLaunch) {
    if (const auto command_line =
            arguments.Data()
                .try_as<Windows::ApplicationModel::Activation::ICommandLineActivatedEventArgs>()) {
      raw_arguments = command_line.Operation().Arguments();
    }
  } else {
    return;
  }

  const auto projected_window = window_.as<winrt::azookey_settings::MainWindow>();
  winrt::get_self<winrt::azookey_settings::implementation::MainWindow>(projected_window)
      ->ApplyLaunchArguments(raw_arguments);
}

void App::RestoreAndActivateWindow() {
  HWND window_handle = nullptr;
  winrt::check_hresult(window_.as<::IWindowNative>()->get_WindowHandle(&window_handle));
  ShowWindow(window_handle, SW_RESTORE);
  SetForegroundWindow(window_handle);
}

}  // namespace winrt::azookey_settings::implementation
