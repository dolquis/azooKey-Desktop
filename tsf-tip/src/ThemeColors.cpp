#include "azookey/tsf/ThemeColors.h"

#include <dwmapi.h>

#include <cwchar>

namespace azookey::tsf {
namespace {

// DWMWA_USE_IMMERSIVE_DARK_MODE; older SDK headers lack the name. Windows 10
// 1809-1909 used the undocumented value 19 for the same attribute.
constexpr DWORD kUseImmersiveDarkModeAttribute = 20;
constexpr DWORD kUseImmersiveDarkModeAttributeBefore20H1 = 19;
constexpr wchar_t kImmersiveColorSet[] = L"ImmersiveColorSet";

#ifdef AZOOKEY_TSF_TESTING
struct ThemeInputs {
  bool high_contrast;
  std::optional<DWORD> apps_use_light_theme;
};
std::optional<ThemeInputs> g_theme_inputs_for_test;
#endif

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
#ifdef AZOOKEY_TSF_TESTING
  if (g_theme_inputs_for_test) {
    return ThemeModeFrom(g_theme_inputs_for_test->high_contrast,
                         g_theme_inputs_for_test->apps_use_light_theme);
  }
#endif
  return ThemeModeFrom(HighContrastEnabled(), ReadAppsUseLightTheme());
}

bool IsThemeSettingChange(WPARAM wparam, LPARAM lparam) {
  if (wparam == SPI_SETHIGHCONTRAST) return true;
  if (wparam != 0 || !lparam) return false;
  // wcsncmp stops at the first difference, so at most the name's length is read.
  return std::wcsncmp(reinterpret_cast<const wchar_t*>(lparam), kImmersiveColorSet,
                      sizeof(kImmersiveColorSet) / sizeof(wchar_t)) == 0;
}

void ApplyWindowFrameTheme(HWND hwnd, ThemeMode mode) {
  if (!hwnd) return;
  const BOOL dark = mode == ThemeMode::Dark ? TRUE : FALSE;
  if (FAILED(DwmSetWindowAttribute(hwnd, kUseImmersiveDarkModeAttribute, &dark, sizeof(dark)))) {
    DwmSetWindowAttribute(hwnd, kUseImmersiveDarkModeAttributeBefore20H1, &dark, sizeof(dark));
  }
}

#ifdef AZOOKEY_TSF_TESTING
namespace testing {
void SetThemeInputsForTest(bool high_contrast, std::optional<DWORD> apps_use_light_theme) {
  g_theme_inputs_for_test = ThemeInputs{high_contrast, apps_use_light_theme};
}
void ClearThemeInputsForTest() { g_theme_inputs_for_test.reset(); }
}  // namespace testing
#endif

}  // namespace azookey::tsf
