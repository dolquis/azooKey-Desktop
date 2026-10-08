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

TEST(PredictionWindowTest, PlacesToRightOfCaretWhenThereIsRoom) {
  const RECT caret{100, 200, 102, 220};
  const RECT work{0, 0, 1000, 800};
  const RECT result = PredictionWindow::ComputePlacement(caret, work, 180, 140);
  EXPECT_EQ(result.left, 106);
  EXPECT_EQ(result.top, 200);
  EXPECT_EQ(result.right, 286);
  EXPECT_EQ(result.bottom, 340);
}

TEST(PredictionWindowTest, FallsBackLeftWhenRightWouldOverflow) {
  const RECT result =
      PredictionWindow::ComputePlacement({780, 200, 782, 220}, {0, 0, 800, 600}, 180, 140);
  EXPECT_EQ(result.left, 596);
  EXPECT_EQ(result.top, 200);
}

TEST(PredictionWindowTest, FallsBackAboveWhenBottomWouldOverflow) {
  const RECT result =
      PredictionWindow::ComputePlacement({100, 570, 102, 590}, {0, 0, 800, 600}, 180, 140);
  EXPECT_EQ(result.left, 106);
  EXPECT_EQ(result.top, 450);
}

TEST(PredictionWindowTest, ClampsToNonPrimaryMonitorWorkAreaWhenNeitherSideFits) {
  const RECT result = PredictionWindow::ComputePlacement({-1750, 120, -1748, 140},
                                                         {-1920, 40, -1720, 200}, 180, 140);
  EXPECT_EQ(result.left, -1920);
  EXPECT_EQ(result.top, 40);
  EXPECT_EQ(result.right, -1740);
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

TEST(PredictionWindowTest, DpiChangeRescalesTheVisibleWindow) {
  // Read sizes in physical pixels whatever this test thread's DPI awareness is.
  const ScopedPerMonitorDpiAwareness dpi_context;
  PredictionWindow window;
  ASSERT_TRUE(window.Create()) << window.failure_stage();
  window.Show({L"日本"}, RECT{100, 200, 101, 216});
  ASSERT_TRUE(window.IsVisible()) << window.failure_stage();

  // The suggested rect is ignored; the window is re-shown for the caret's monitor.
  RECT suggested{100, 216, 300, 300};
  SendMessageW(window.hwnd_for_test(), WM_DPICHANGED, MAKEWPARAM(144, 144),
               reinterpret_cast<LPARAM>(&suggested));

  RECT client{};
  ASSERT_TRUE(GetClientRect(window.hwnd_for_test(), &client));
  // One row: padding * 2 + row height at the window's DPI.
  const UINT dpi = GetDpiForWindow(window.hwnd_for_test());
  EXPECT_EQ(client.bottom - client.top, ScaleForDpi(8, dpi) * 2 + ScaleForDpi(28, dpi));
  RECT bounds{};
  ASSERT_TRUE(GetWindowRect(window.hwnd_for_test(), &bounds));
  EXPECT_NE(bounds.right - bounds.left, suggested.right - suggested.left);
  EXPECT_TRUE(window.IsVisible()) << window.failure_stage();
  window.Hide();
}

TEST(PredictionWindowTest, CreateResolvesTheCurrentTheme) {
  PredictionWindow window;
  ASSERT_TRUE(window.Create()) << window.failure_stage();
  const ThemeColors expected = ResolveThemeColors(CurrentThemeMode());
  EXPECT_EQ(window.theme_for_test().background, expected.background);
  EXPECT_EQ(window.theme_for_test().text, expected.text);
}

}  // namespace
}  // namespace azookey::tsf
