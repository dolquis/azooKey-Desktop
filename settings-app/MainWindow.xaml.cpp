// C++/WinRT requires the precompiled header before generated XAML headers.
// clang-format off
#include "pch.h"
#include "MainWindow.xaml.h"
#include "LaunchArguments.h"
// clang-format on

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <array>

namespace {

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

MainWindow::MainWindow() { InitializeComponent(); }

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
