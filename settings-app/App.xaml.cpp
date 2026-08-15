#include "pch.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"

namespace winrt::azookey_settings::implementation {

App::App() { InitializeComponent(); }

App::~App() {
  if (instance_mutex_ != nullptr) {
    CloseHandle(instance_mutex_);
  }
}

void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) {
  instance_mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\azooKey.Settings.Singleton");
  if (instance_mutex_ != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND existing = FindWindowW(nullptr, L"azooKey Settings")) {
      ShowWindow(existing, SW_RESTORE);
      SetForegroundWindow(existing);
    }
    Exit();
    return;
  }

  window_ = winrt::make<MainWindow>();
  window_.Title(L"azooKey Settings");
  window_.Activate();
}

}  // namespace winrt::azookey_settings::implementation
