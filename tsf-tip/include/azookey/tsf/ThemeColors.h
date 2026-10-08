#pragma once

#include <Windows.h>

#include <optional>

namespace azookey::tsf {

// How the candidate and prediction windows are colored (native-ui-spec §1, §5.1).
enum class ThemeMode : unsigned char { Light, Dark, HighContrast };

struct ThemeColors {
  COLORREF background;
  COLORREF text;
  COLORREF sub_text;  // Descriptions and disabled buttons.
  COLORREF selection;
  COLORREF selection_text;
  COLORREF border;
  COLORREF panel_background;  // Notice row.
  COLORREF panel_text;
  COLORREF info_background;  // Secure toast and health details popup.
  COLORREF info_text;
  COLORREF banner_background;  // Degraded health banner.
};

inline constexpr ThemeColors kLightTheme{
    RGB(255, 255, 255), RGB(0, 0, 0),       RGB(96, 96, 96),    RGB(0, 120, 215),
    RGB(255, 255, 255), RGB(160, 160, 160), RGB(243, 243, 243), RGB(0, 0, 0),
    RGB(255, 255, 225), RGB(0, 0, 0),       RGB(255, 249, 225),
};

inline constexpr ThemeColors kDarkTheme{
    RGB(32, 32, 32), RGB(255, 255, 255), RGB(160, 160, 160), RGB(76, 194, 255),
    RGB(0, 0, 0),    RGB(80, 80, 80),    RGB(45, 45, 45),    RGB(255, 255, 255),
    RGB(56, 56, 40), RGB(255, 255, 255), RGB(67, 53, 25),
};

using SysColorFn = DWORD(WINAPI*)(int);

// High contrast wins over the app theme; a missing AppsUseLightTheme value means Light.
ThemeMode ThemeModeFrom(bool high_contrast, std::optional<DWORD> apps_use_light_theme);

// The fixed table for Light / Dark, or system colors under high contrast.
ThemeColors ResolveThemeColors(ThemeMode mode, SysColorFn sys_color = &::GetSysColor);

// Reads SPI_GETHIGHCONTRAST and HKCU ...\Themes\Personalize\AppsUseLightTheme.
ThemeMode CurrentThemeMode();

// True for the WM_SETTINGCHANGE that announces a light/dark switch.
bool IsThemeSettingChange(LPARAM lparam);

// Asks DWM to draw the window frame dark or light. Failures are ignored: the
// attribute does not exist before Windows 10 20H1.
void ApplyWindowFrameTheme(HWND hwnd, ThemeMode mode);

}  // namespace azookey::tsf
