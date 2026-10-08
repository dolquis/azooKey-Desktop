#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <gtest/gtest.h>

#include "azookey/tsf/DpiScaling.h"
#include "azookey/tsf/RenderingEngine.h"
#include "azookey/tsf/ThemeColors.h"

namespace azookey::tsf {
namespace {

DWORD WINAPI FakeSysColor(int index) { return RGB(index, index + 1, index + 2); }

TEST(ThemeColorsTest, HighContrastWinsOverTheAppTheme) {
  EXPECT_EQ(ThemeModeFrom(true, 0u), ThemeMode::HighContrast);
  EXPECT_EQ(ThemeModeFrom(true, std::nullopt), ThemeMode::HighContrast);
}

TEST(ThemeColorsTest, AppsUseLightThemeZeroMeansDarkAndMissingMeansLight) {
  EXPECT_EQ(ThemeModeFrom(false, 0u), ThemeMode::Dark);
  EXPECT_EQ(ThemeModeFrom(false, 1u), ThemeMode::Light);
  EXPECT_EQ(ThemeModeFrom(false, std::nullopt), ThemeMode::Light);
}

TEST(ThemeColorsTest, LightAndDarkUseTheFixedTables) {
  const ThemeColors light = ResolveThemeColors(ThemeMode::Light, &FakeSysColor);
  EXPECT_EQ(light.background, RGB(255, 255, 255));
  EXPECT_EQ(light.text, RGB(0, 0, 0));
  EXPECT_EQ(light.selection, RGB(0, 120, 215));
  EXPECT_EQ(light.sub_text, RGB(96, 96, 96));

  const ThemeColors dark = ResolveThemeColors(ThemeMode::Dark, &FakeSysColor);
  EXPECT_EQ(dark.background, RGB(32, 32, 32));
  EXPECT_EQ(dark.text, RGB(255, 255, 255));
  EXPECT_EQ(dark.selection, RGB(76, 194, 255));
  EXPECT_EQ(dark.sub_text, RGB(160, 160, 160));
}

TEST(ThemeColorsTest, EveryThemeKeepsTextDistinctFromItsBackground) {
  for (const ThemeColors& theme : {kLightTheme, kDarkTheme}) {
    EXPECT_NE(theme.text, theme.background);
    EXPECT_NE(theme.selection_text, theme.selection);
    EXPECT_NE(theme.panel_text, theme.panel_background);
    EXPECT_NE(theme.info_text, theme.info_background);
    EXPECT_NE(theme.text, theme.banner_background);
  }
}

TEST(ThemeColorsTest, HighContrastUsesSystemColors) {
  const ThemeColors colors = ResolveThemeColors(ThemeMode::HighContrast, &FakeSysColor);
  EXPECT_EQ(colors.background, FakeSysColor(COLOR_WINDOW));
  EXPECT_EQ(colors.text, FakeSysColor(COLOR_WINDOWTEXT));
  EXPECT_EQ(colors.sub_text, FakeSysColor(COLOR_GRAYTEXT));
  EXPECT_EQ(colors.selection, FakeSysColor(COLOR_HIGHLIGHT));
  EXPECT_EQ(colors.selection_text, FakeSysColor(COLOR_HIGHLIGHTTEXT));
  EXPECT_EQ(colors.panel_background, FakeSysColor(COLOR_BTNFACE));
  EXPECT_EQ(colors.info_background, FakeSysColor(COLOR_INFOBK));
  EXPECT_EQ(colors.info_text, FakeSysColor(COLOR_INFOTEXT));
}

TEST(ThemeColorsTest, OnlyImmersiveColorSetAndHighContrastAreThemeSettingChanges) {
  EXPECT_TRUE(IsThemeSettingChange(0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet")));
  EXPECT_TRUE(IsThemeSettingChange(SPI_SETHIGHCONTRAST, 0));
  EXPECT_FALSE(IsThemeSettingChange(0, reinterpret_cast<LPARAM>(L"Policy")));
  EXPECT_FALSE(IsThemeSettingChange(0, reinterpret_cast<LPARAM>(L"ImmersiveColorSetX")));
  EXPECT_FALSE(IsThemeSettingChange(0, 0));
  // A nonzero wParam is an SPI_SET* code; its lParam is not read as a string.
  EXPECT_FALSE(IsThemeSettingChange(SPI_SETWORKAREA, 1));
}

TEST(ThemeColorsTest, InjectedInputsDriveCurrentThemeMode) {
  testing::SetThemeInputsForTest(false, 0u);
  EXPECT_EQ(CurrentThemeMode(), ThemeMode::Dark);
  testing::SetThemeInputsForTest(true, 0u);
  EXPECT_EQ(CurrentThemeMode(), ThemeMode::HighContrast);
  testing::SetThemeInputsForTest(false, std::nullopt);
  EXPECT_EQ(CurrentThemeMode(), ThemeMode::Light);
  testing::ClearThemeInputsForTest();
}

TEST(ThemeColorsTest, CurrentThemeModeReadsTheSystemWithoutFailing) {
  const ThemeMode mode = CurrentThemeMode();
  EXPECT_TRUE(mode == ThemeMode::Light || mode == ThemeMode::Dark ||
              mode == ThemeMode::HighContrast);
}

TEST(DpiScalingTest, ScalesDesignValuesAt96And144And192Dpi) {
  EXPECT_EQ(ScaleForDpi(28, 96), 28);
  EXPECT_EQ(ScaleForDpi(28, 144), 42);
  EXPECT_EQ(ScaleForDpi(28, 192), 56);
  EXPECT_EQ(ScaleForDpi(14, 0), 14);
  EXPECT_EQ(NormalizeDpi(0), USER_DEFAULT_SCREEN_DPI);
  EXPECT_EQ(NormalizeDpi(144), 144u);
}

TEST(DpiScalingTest, ScopedAwarenessRestoresThePreviousContext) {
  const DPI_AWARENESS_CONTEXT before = GetThreadDpiAwarenessContext();
  {
    const ScopedPerMonitorDpiAwareness scope;
    EXPECT_TRUE(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                             DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
  }
  EXPECT_TRUE(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), before));
}

TEST(RenderingEngineTest, DrawsAndCommitsToACompositionSurface) {
  HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
                              L"STATIC", nullptr, WS_POPUP, 0, 0, 64, 32, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
  ASSERT_NE(hwnd, nullptr);
  RenderingEngine engine;
  EXPECT_EQ(engine.BeginDraw(), nullptr);
  EXPECT_STREQ(engine.failure_stage(), "no_render");

  ASSERT_TRUE(engine.Initialize(hwnd)) << engine.failure_stage() << " hr=0x" << std::hex
                                       << static_cast<unsigned long>(engine.failure_hr());
  ASSERT_NE(engine.write_factory(), nullptr);
  ASSERT_TRUE(engine.ResizeSurface(64, 32));
  ID2D1DeviceContext* context = engine.BeginDraw();
  ASSERT_NE(context, nullptr);
  context->Clear(D2D1::ColorF(0.125f, 0.125f, 0.125f, 1.0f));
  EXPECT_TRUE(engine.EndDraw()) << engine.failure_stage();

  engine.Reset();
  EXPECT_FALSE(engine.IsInitialized());
  DestroyWindow(hwnd);
}

}  // namespace
}  // namespace azookey::tsf
