#include <gtest/gtest.h>

#include "azookey/tsf/CandidateWindow.h"

namespace azookey::tsf {
namespace {

class ScopedPerMonitorDpi {
 public:
  ScopedPerMonitorDpi()
      : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
  ~ScopedPerMonitorDpi() {
    if (previous_) SetThreadDpiAwarenessContext(previous_);
  }
  bool valid() const { return previous_ != nullptr; }

 private:
  DPI_AWARENESS_CONTEXT previous_{nullptr};
};

void ExpectMetrics(const CandidateWindow::LayoutMetricsForTest& metrics, int item_height,
                   int horizontal_padding, int max_width, int caret_gap, int min_text_width,
                   int extra_width) {
  EXPECT_EQ(metrics.item_height, item_height);
  EXPECT_EQ(metrics.horizontal_padding, horizontal_padding);
  EXPECT_EQ(metrics.max_width, max_width);
  EXPECT_EQ(metrics.caret_gap, caret_gap);
  EXPECT_EQ(metrics.min_text_width, min_text_width);
  EXPECT_EQ(metrics.extra_width, extra_width);
}

TEST(CandidateWindowDpiTest, LayoutMetricsScaleFromDefaultDpi) {
  ExpectMetrics(CandidateWindow::ComputeLayoutMetricsForTest(96), 24, 8, 400, 20, 60, 4);
  ExpectMetrics(CandidateWindow::ComputeLayoutMetricsForTest(144), 36, 12, 600, 30, 90, 6);
  ExpectMetrics(CandidateWindow::ComputeLayoutMetricsForTest(192), 48, 16, 800, 40, 120, 8);
}

TEST(CandidateWindowDpiTest, EmojiDetectionDoesNotReclassifyKanjiOrTextSymbols) {
  for (const auto* text : {L"𠮟", L"𩸽", L"★☆♪✓✂☀", L"😄︎", L"abc"})
    EXPECT_FALSE(CandidateWindow::NeedsColorEmoji(text));
  for (const auto* text : {L"😄", L"☀️", L"👩‍💻", L"🇯🇵", L"1️⃣"})
    EXPECT_TRUE(CandidateWindow::NeedsColorEmoji(text));
}

TEST(CandidateWindowDpiTest, CreateResolvesTheCurrentThemeAndFollowsAThemeChange) {
  CandidateWindow window;
  ASSERT_TRUE(window.Create());
  const ThemeColors expected = ResolveThemeColors(CurrentThemeMode());
  EXPECT_EQ(window.theme_for_test().background, expected.background);
  EXPECT_EQ(window.theme_for_test().selection, expected.selection);
  // An unrelated setting change keeps the colors; ImmersiveColorSet re-reads them.
  SendMessageW(window.hwnd_for_test(), WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Policy"));
  SendMessageW(window.hwnd_for_test(), WM_SETTINGCHANGE, 0,
               reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
  EXPECT_EQ(window.theme_for_test().text, ResolveThemeColors(CurrentThemeMode()).text);
  window.Destroy();
}

TEST(CandidateWindowDpiTest, ZeroDpiFallsBackToDefaultDpi) {
  ExpectMetrics(CandidateWindow::ComputeLayoutMetricsForTest(0), 24, 8, 400, 20, 60, 4);
}

void ExpectRect(const RECT& rect, LONG left, LONG top, LONG right, LONG bottom) {
  EXPECT_EQ(rect.left, left);
  EXPECT_EQ(rect.top, top);
  EXPECT_EQ(rect.right, right);
  EXPECT_EQ(rect.bottom, bottom);
}

TEST(CandidateWindowPlacementTest, OpensBelowTheAnchorWhenThereIsRoom) {
  ExpectRect(CandidateWindow::ComputePlacement({100, 200}, {0, 0, 1920, 1040}, 300, 120, 20), 100,
             200, 400, 320);
}

TEST(CandidateWindowPlacementTest, FlipsAboveTheCaretWhenTheBottomWouldOverflow) {
  ExpectRect(CandidateWindow::ComputePlacement({100, 1000}, {0, 0, 1920, 1040}, 300, 120, 20), 100,
             860, 400, 980);
}

TEST(CandidateWindowPlacementTest, ClampsToTheRightEdgeOfTheWorkArea) {
  ExpectRect(CandidateWindow::ComputePlacement({1800, 200}, {0, 0, 1920, 1040}, 300, 120, 20), 1620,
             200, 1920, 320);
}

TEST(CandidateWindowPlacementTest, StaysOnASecondaryMonitorAtNegativeCoordinates) {
  const RECT work{-1280, 40, 0, 1024};
  ExpectRect(CandidateWindow::ComputePlacement({-100, 300}, work, 300, 120, 20), -300, 300, 0, 420);
  ExpectRect(CandidateWindow::ComputePlacement({-1400, 50}, work, 300, 1100, 20), -1280, 40, -980,
             1140);
}

TEST(CandidateWindowPlacementTest, NarrowsToTheWorkAreaWidth) {
  ExpectRect(CandidateWindow::ComputePlacement({50, 100}, {0, 0, 200, 600}, 300, 120, 20), 0, 100,
             200, 220);
}

TEST(CandidateWindowPlacementTest, EmptyWorkAreaKeepsTheAnchor) {
  ExpectRect(CandidateWindow::ComputePlacement({100, 200}, {}, 300, 120, 20), 100, 200, 400, 320);
}

TEST(CandidateWindowPlacementTest, DetailsOpenBelowOrAboveTheCandidateWindow) {
  const RECT work{0, 0, 1920, 1040};
  ExpectRect(CandidateWindow::ComputeDetailsPlacement({100, 200, 400, 320}, work, 480, 240), 100,
             320, 580, 560);
  ExpectRect(CandidateWindow::ComputeDetailsPlacement({1700, 900, 1900, 1000}, work, 480, 240),
             1440, 660, 1920, 900);
  ExpectRect(CandidateWindow::ComputeDetailsPlacement({10, 10, 50, 50}, {0, 0, 300, 200}, 480, 240),
             0, 0, 300, 200);
}

TEST(CandidateWindowDpiTest, NoDescriptionKeepsLegacySingleColumnWidth) {
  const auto layout = CandidateWindow::ComputeColumnLayoutForTest(380, 0, 96);
  EXPECT_EQ(layout.surface_width, 0);
  EXPECT_EQ(layout.column_gap, 0);
  EXPECT_EQ(layout.content_width, 380);
}

TEST(CandidateWindowDpiTest, DescriptionEnablesClampedTwoColumnLayout) {
  const auto layout = CandidateWindow::ComputeColumnLayoutForTest(380, 80, 96);
  EXPECT_EQ(layout.surface_width, 220);
  EXPECT_EQ(layout.column_gap, 12);
  EXPECT_EQ(layout.content_width, 312);
}

TEST(CandidateWindowDpiTest, NoticeSurvivesHealthBannerResizeAndKeepsClickRegionsSeparate) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  CandidateWindow window;
  ASSERT_TRUE(window.Create());
  int candidate_clicks = 0;
  int retries = 0;
  window.SetOnClick([&](int) { ++candidate_clicks; });
  window.SetOnRetry([&] { ++retries; });
  window.Show(POINT{20, 20}, {{L"候補", L""}}, 0, L"AI 整文の案内");
  const HWND hwnd = window.hwnd_for_test();
  const auto metrics = window.current_metrics_for_test();
  RECT bounds{};
  ASSERT_TRUE(GetWindowRect(hwnd, &bounds));
  EXPECT_EQ(bounds.bottom - bounds.top, 2 * metrics.item_height);

  window.ShowHealthBanner(CandidateHealthState::DegradedModel);
  ASSERT_TRUE(GetWindowRect(hwnd, &bounds));
  EXPECT_EQ(bounds.bottom - bounds.top, 5 * metrics.item_height);
  EXPECT_EQ(window.notice_for_test(), L"AI 整文の案内");
  const int width = bounds.right - bounds.left;
  const int click_x = width - metrics.horizontal_padding - 10;
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(click_x, metrics.item_height + metrics.item_height / 2));
  EXPECT_EQ(candidate_clicks, 0);
  EXPECT_EQ(retries, 0);
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(click_x, 4 * metrics.item_height + metrics.item_height / 2));
  EXPECT_EQ(retries, 1);

  window.HideHealthBanner();
  ASSERT_TRUE(GetWindowRect(hwnd, &bounds));
  EXPECT_EQ(bounds.bottom - bounds.top, 2 * metrics.item_height);
  EXPECT_EQ(window.notice_for_test(), L"AI 整文の案内");
  window.Destroy();
}

TEST(CandidateWindowDpiTest, SecureToastShiftsHealthButtonsAndLockIsNotACandidate) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  CandidateWindow window;
  ASSERT_TRUE(window.Create());
  int candidate_clicks = 0;
  int retries = 0;
  window.SetOnClick([&](int) { ++candidate_clicks; });
  window.SetOnRetry([&] { ++retries; });
  window.Show(POINT{20, 20}, {{L"候補", L""}}, 0);
  const HWND hwnd = window.hwnd_for_test();
  const auto metrics = window.current_metrics_for_test();
  RECT bounds{};
  ASSERT_TRUE(GetWindowRect(hwnd, &bounds));
  const int plain_width = bounds.right - bounds.left;

  window.SetSecureIndicator(true);
  ASSERT_TRUE(GetWindowRect(hwnd, &bounds));
  EXPECT_GT(bounds.right - bounds.left, plain_width);
  RECT client{};
  ASSERT_TRUE(GetClientRect(hwnd, &client));
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(client.right - 2, 2));
  EXPECT_EQ(candidate_clicks, 0);
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, 2));
  EXPECT_EQ(candidate_clicks, 1);

  window.ShowSecureToast();
  window.ShowHealthBanner(CandidateHealthState::DegradedModel);
  ASSERT_TRUE(GetWindowRect(hwnd, &bounds));
  EXPECT_EQ(bounds.bottom - bounds.top, 5 * metrics.item_height);
  ASSERT_TRUE(GetClientRect(hwnd, &client));
  const int click_x = client.right - metrics.horizontal_padding - 10;
  // Without the toast row this would be the [再試行] row; now it is banner text.
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(click_x, 3 * metrics.item_height + metrics.item_height / 2));
  EXPECT_EQ(retries, 0);
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(click_x, 4 * metrics.item_height + metrics.item_height / 2));
  EXPECT_EQ(retries, 1);
  EXPECT_EQ(candidate_clicks, 1);

  window.HideSecureToast();
  window.HideHealthBanner();
  ASSERT_TRUE(GetWindowRect(hwnd, &bounds));
  EXPECT_EQ(bounds.bottom - bounds.top, metrics.item_height);
  window.Destroy();
}

}  // namespace
}  // namespace azookey::tsf
