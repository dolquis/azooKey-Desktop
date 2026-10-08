#include "azookey/tsf/ThemeColors.h"

#include <dwmapi.h>

#include <cwchar>

namespace azookey::tsf {
namespace {

// DWMWA_USE_IMMERSIVE_DARK_MODE; older SDK headers lack the name.
constexpr DWORD kUseImmersiveDarkModeAttribute = 20;

std::optional<DWORD> ReadAppsUseLightTheme() {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (RegGetValueW(
          HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  return value;
}

bool HighContrastEnabled() {
  HIGHCONTRASTW high_contrast{};
  high_contrast.cbSize = sizeof(high_contrast);
  return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(high_contrast), &high_contrast, 0) &&
         (high_contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

}  // namespace

ThemeMode ThemeModeFrom(bool high_contrast, std::optional<DWORD> apps_use_light_theme) {
  if (high_contrast) return ThemeMode::HighContrast;
  return apps_use_light_theme && *apps_use_light_theme == 0 ? ThemeMode::Dark : ThemeMode::Light;
}

ThemeColors ResolveThemeColors(ThemeMode mode, SysColorFn sys_color) {
  if (mode == ThemeMode::Dark) return kDarkTheme;
  if (mode == ThemeMode::Light || !sys_color) return kLightTheme;
  // High contrast keeps the user's system colors (native-ui-spec §5.1).
  return {
      sys_color(COLOR_WINDOW),    sys_color(COLOR_WINDOWTEXT),    sys_color(COLOR_GRAYTEXT),
      sys_color(COLOR_HIGHLIGHT), sys_color(COLOR_HIGHLIGHTTEXT), sys_color(COLOR_WINDOWTEXT),
      sys_color(COLOR_BTNFACE),   sys_color(COLOR_BTNTEXT),       sys_color(COLOR_INFOBK),
      sys_color(COLOR_INFOTEXT),  sys_color(COLOR_INFOBK),
  };
}

ThemeMode CurrentThemeMode() {
  return ThemeModeFrom(HighContrastEnabled(), ReadAppsUseLightTheme());
}

bool IsThemeSettingChange(LPARAM lparam) {
  const auto* area = reinterpret_cast<const wchar_t*>(lparam);
  return area && std::wcscmp(area, L"ImmersiveColorSet") == 0;
}

void ApplyWindowFrameTheme(HWND hwnd, ThemeMode mode) {
  if (!hwnd) return;
  const BOOL dark = mode == ThemeMode::Dark ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd, kUseImmersiveDarkModeAttribute, &dark, sizeof(dark));
}

}  // namespace azookey::tsf
