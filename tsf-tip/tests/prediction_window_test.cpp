#include <d2d1.h>
#include <dxgi.h>
#include <gtest/gtest.h>

#include "azookey/tsf/DpiScaling.h"
#include "azookey/tsf/PredictionWindow.h"

namespace azookey::tsf {
namespace {

TEST(PredictionWindowTest, CapsVisibleRowsAtFive) {
  EXPECT_EQ(PredictionWindow::VisibleCount(0), 0u);
  EXPECT_EQ(PredictionWindow::VisibleCount(2), 2u);
  EXPECT_EQ(PredictionWindow::VisibleCount(5), 5u);
  EXPECT_EQ(PredictionWindow::VisibleCount(9), 5u);
}

TEST(PredictionWindowTest, PlacesBelowTheCompositionStartWhenThereIsRoom) {
  const RECT composition{100, 200, 160, 220};
  const RECT work{0, 0, 1000, 800};
  const RECT result = PredictionWindow::ComputePlacement(composition, work, 180, 140);
  EXPECT_EQ(result.left, 100);
  EXPECT_EQ(result.top, 220);
  EXPECT_EQ(result.right, 280);
  EXPECT_EQ(result.bottom, 360);
}

// DEV-1732: typing only moves the composition's right edge; the window stays.
TEST(PredictionWindowTest, StaysPutWhileTheCompositionGrows) {
  const RECT work{0, 0, 800, 600};
  const RECT first = PredictionWindow::ComputePlacement({100, 200, 116, 220}, work, 180, 140);
  for (const LONG right : {132L, 260L, 700L}) {
    const RECT next = PredictionWindow::ComputePlacement({100, 200, right, 220}, work, 180, 140);
    EXPECT_EQ(next.left, first.left);
    EXPECT_EQ(next.top, first.top);
  }
  const RECT near_bottom = PredictionWindow::ComputePlacement({100, 570, 116, 590}, work, 180, 140);
  const RECT grown = PredictionWindow::ComputePlacement({100, 570, 700, 590}, work, 180, 140);
  EXPECT_EQ(grown.left, near_bottom.left);
  EXPECT_EQ(grown.top, near_bottom.top);
}

TEST(PredictionWindowTest, ShiftsLeftToStayInsideTheWorkArea) {
  const RECT result =
      PredictionWindow::ComputePlacement({780, 200, 790, 220}, {0, 0, 800, 600}, 180, 140);
  EXPECT_EQ(result.left, 620);
  EXPECT_EQ(result.top, 220);
}

TEST(PredictionWindowTest, FallsBackAboveWhenBottomWouldOverflow) {
  const RECT result =
      PredictionWindow::ComputePlacement({100, 570, 160, 590}, {0, 0, 800, 600}, 180, 140);
  EXPECT_EQ(result.left, 100);
  EXPECT_EQ(result.top, 430);
}

TEST(PredictionWindowTest, ClampsToNonPrimaryMonitorWorkAreaWhenNeitherBelowNorAboveFits) {
  const RECT result = PredictionWindow::ComputePlacement({-1750, 120, -1748, 140},
                                                         {-1920, 40, -1720, 200}, 180, 140);
  EXPECT_EQ(result.left, -1900);
  EXPECT_EQ(result.top, 40);
  EXPECT_EQ(result.right, -1720);
  EXPECT_EQ(result.bottom, 180);
}

TEST(PredictionWindowTest, CreateAndShowMakeTheWindowVisible) {
  // Create used to fail at the DirectComposition device on every call, so the
  // window never appeared even with predictions in hand.
  PredictionWindow window;
  ASSERT_TRUE(window.Create()) << window.failure_stage() << " hr=0x" << std::hex
                               << static_cast<unsigned long>(window.failure_hr());
  window.Show({L"日本", L"日本語"}, RECT{100, 200, 101, 216});
  EXPECT_TRUE(window.IsVisible()) << window.failure_stage() << " hr=0x" << std::hex
                                  << static_cast<unsigned long>(window.failure_hr());
  window.Hide();
  EXPECT_FALSE(window.IsVisible());
}

TEST(PredictionWindowTest, DpiChangeRescalesTheVisibleWindowAt96And144And192) {
  // Read sizes in physical pixels whatever this test thread's DPI awareness is.
  const ScopedPerMonitorDpiAwareness dpi_context;
  PredictionWindow window;
  ASSERT_TRUE(window.Create()) << window.failure_stage();
  PredictionWindow::SetMonitorDpiForTest(96);
  window.Show({L"日本"}, RECT{100, 200, 101, 216});
  ASSERT_TRUE(window.IsVisible()) << window.failure_stage();

  // One row is padding * 2 + row height, both scaled from 8 and 28 at 96 DPI.
  const auto client_height = [&window] {
    RECT client{};
    EXPECT_TRUE(GetClientRect(window.hwnd_for_test(), &client));
    return client.bottom - client.top;
  };
  EXPECT_EQ(window.row_height_for_test(), 28);
  EXPECT_EQ(client_height(), 44);

  for (const UINT dpi : {144u, 192u}) {
    SCOPED_TRACE(dpi);
    PredictionWindow::SetMonitorDpiForTest(dpi);
    // The suggested rect is ignored; the window is re-shown for the caret's monitor.
    RECT suggested{100, 216, 300, 300};
    SendMessageW(window.hwnd_for_test(), WM_DPICHANGED, MAKEWPARAM(dpi, dpi),
                 reinterpret_cast<LPARAM>(&suggested));
    EXPECT_EQ(window.row_height_for_test(), ScaleForDpi(28, dpi));
    EXPECT_EQ(client_height(), ScaleForDpi(8, dpi) * 2 + ScaleForDpi(28, dpi));
    RECT bounds{};
    ASSERT_TRUE(GetWindowRect(window.hwnd_for_test(), &bounds));
    EXPECT_NE(bounds.right - bounds.left, suggested.right - suggested.left);
    EXPECT_TRUE(window.IsVisible()) << window.failure_stage();
  }
  window.Hide();
  PredictionWindow::SetMonitorDpiForTest(0);
}

TEST(PredictionWindowTest, ThemeFollowsTheSystemSettingChanges) {
  testing::SetThemeInputsForTest(false, 1u);
  PredictionWindow window;
  ASSERT_TRUE(window.Create()) << window.failure_stage();
  EXPECT_EQ(window.theme_for_test().background, kLightTheme.background);

  testing::SetThemeInputsForTest(false, 0u);
  SendMessageW(window.hwnd_for_test(), WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Policy"));
  EXPECT_EQ(window.theme_for_test().background, kLightTheme.background);
  SendMessageW(window.hwnd_for_test(), WM_SETTINGCHANGE, 0,
               reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
  EXPECT_EQ(window.theme_for_test().background, kDarkTheme.background);

  testing::SetThemeInputsForTest(true, 0u);
  SendMessageW(window.hwnd_for_test(), WM_SETTINGCHANGE, SPI_SETHIGHCONTRAST, 0);
  EXPECT_EQ(window.theme_for_test().background, GetSysColor(COLOR_WINDOW));
  testing::ClearThemeInputsForTest();
}

TEST(PredictionWindowTest, LostDevicesAreRecognized) {
  EXPECT_TRUE(PredictionWindow::IsDeviceLostForTest(DXGI_ERROR_DEVICE_REMOVED));
  EXPECT_TRUE(PredictionWindow::IsDeviceLostForTest(DXGI_ERROR_DEVICE_RESET));
  EXPECT_TRUE(PredictionWindow::IsDeviceLostForTest(D2DERR_RECREATE_TARGET));
  EXPECT_FALSE(PredictionWindow::IsDeviceLostForTest(E_OUTOFMEMORY));
}

}  // namespace
}  // namespace azookey::tsf
