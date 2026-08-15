#include "pch.h"

#include "MainWindow.xaml.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <string>
#include <string_view>

namespace winrt::azookey_settings::implementation {

MainWindow::MainWindow() {
  InitializeComponent();

  std::wstring langid = L"(not supplied)";
  std::wstring profile = L"(not supplied)";
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv != nullptr) {
    for (int i = 1; i + 1 < argc; ++i) {
      if (std::wstring_view(argv[i]) == L"--langid") {
        langid = argv[++i];
      } else if (std::wstring_view(argv[i]) == L"--profile") {
        profile = argv[++i];
      }
    }
    LocalFree(argv);
  }

  LanguageText().Text(L"Language: " + langid);
  ProfileText().Text(L"Profile: " + profile);
}

}  // namespace winrt::azookey_settings::implementation
