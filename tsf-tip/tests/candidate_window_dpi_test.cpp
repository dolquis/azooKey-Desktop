#include <dwrite.h>
#include <gtest/gtest.h>
#include <wrl/client.h>

#include <algorithm>
#include <vector>

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

// Restores the injected monitor DPI and theme inputs even when an ASSERT leaves early.
class ScopedTestInputs {
 public:
  ScopedTestInputs() = default;
  ~ScopedTestInputs() {
    CandidateWindow::SetRenderFailureForTest(CandidateWindow::RenderFailureForTest::None);
    CandidateWindow::SetMonitorDpiForTest(0);
    tsf::testing::ClearThemeInputsForTest();
  }
  ScopedTestInputs(const ScopedTestInputs&) = delete;
  ScopedTestInputs& operator=(const ScopedTestInputs&) = delete;
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

TEST(CandidateWindowDpiTest, ThemeFollowsTheSystemSettingChanges) {
  testing::SetThemeInputsForTest(false, 1u);
  CandidateWindow window;
  ASSERT_TRUE(window.Create());
  EXPECT_EQ(window.theme_for_test().background, kLightTheme.background);

  // The app theme changes, but an unrelated setting change does not re-read it.
  testing::SetThemeInputsForTest(false, 0u);
  SendMessageW(window.hwnd_for_test(), WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Policy"));
  EXPECT_EQ(window.theme_for_test().background, kLightTheme.background);
  SendMessageW(window.hwnd_for_test(), WM_SETTINGCHANGE, 0,
               reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
  EXPECT_EQ(window.theme_for_test().background, kDarkTheme.background);
  EXPECT_EQ(window.theme_for_test().selection, kDarkTheme.selection);

  // High contrast wins over the dark app theme and uses the system colors.
  testing::SetThemeInputsForTest(true, 0u);
  SendMessageW(window.hwnd_for_test(), WM_SETTINGCHANGE, SPI_SETHIGHCONTRAST, 0);
  EXPECT_EQ(window.theme_for_test().background, GetSysColor(COLOR_WINDOW));
  EXPECT_EQ(window.theme_for_test().selection, GetSysColor(COLOR_HIGHLIGHT));

  window.Destroy();
  testing::ClearThemeInputsForTest();
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

TEST(CandidateWindowHitTest, ClickRegionsScaleAt96And144And192Dpi) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  const ScopedTestInputs inputs;
  using Target = CandidateWindow::HitTarget;
  for (const UINT dpi : {96u, 144u, 192u}) {
    SCOPED_TRACE(dpi);
    CandidateWindow::SetMonitorDpiForTest(dpi);
    CandidateWindow window;
    ASSERT_TRUE(window.Create());
    window.SetSecureIndicator(true);
    window.Show(POINT{20, 20}, {{L"候補", L"説明"}, {L"二", L""}}, 0, L"案内");
    window.ShowSecureToast();
    window.ShowHealthBanner(CandidateHealthState::DegradedModel);

    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const int row = scale(24);
    ASSERT_EQ(window.current_metrics_for_test().item_height, row);
    RECT client{};
    ASSERT_TRUE(GetClientRect(window.hwnd_for_test(), &client));
    const int width = client.right;
    const auto expect = [&](POINT point, Target target, int index = -1) {
      const auto hit = window.HitTestForTest(point);
      EXPECT_EQ(hit.target, target) << point.x << "," << point.y;
      EXPECT_EQ(hit.index, index) << point.x << "," << point.y;
    };

    // Rows 0 and 1 are candidates; the lock takes the right end of row 0.
    expect({0, 0}, Target::Candidate, 0);
    expect({width - scale(24) - 1, row - 1}, Target::Candidate, 0);
    expect({width - scale(24), 0}, Target::SecureIndicator);
    expect({width - 1, row - 1}, Target::SecureIndicator);
    expect({width - 1, row}, Target::Candidate, 1);
    expect({0, 2 * row - 1}, Target::Candidate, 1);
    // The notice row and the toast row are not clickable.
    expect({0, 2 * row}, Target::None);
    expect({0, 4 * row - 1}, Target::None);
    // The banner starts below the toast; its buttons sit on its third row.
    expect({0, 4 * row}, Target::HealthBanner);
    const int button_top = 6 * row;
    const int retry_right = width - scale(8);
    expect({retry_right - scale(60), button_top}, Target::HealthRetryButton);
    expect({retry_right - 1, button_top + row - 1}, Target::HealthRetryButton);
    expect({retry_right, button_top}, Target::HealthBanner);
    expect({retry_right - 1, button_top - 1}, Target::HealthBanner);
    const int details_right = retry_right - scale(68);
    expect({details_right - scale(52), button_top}, Target::HealthDetailsButton);
    expect({details_right - 1, button_top + row - 1}, Target::HealthDetailsButton);
    expect({details_right - scale(52) - 1, button_top}, Target::HealthBanner);
    expect({details_right, button_top}, Target::HealthBanner);
    window.Destroy();
  }
}

TEST(CandidateWindowHitTest, DetailsButtonMovesRightWithoutTheRetryButton) {
  CandidateWindow::HitLayout layout{
      24, 1, 400, 0, true, 24, {340, 72, 392, 96}, false, {332, 72, 392, 96}};
  EXPECT_EQ(CandidateWindow::HitTest(layout, {391, 72}).target,
            CandidateWindow::HitTarget::HealthDetailsButton);
  EXPECT_EQ(CandidateWindow::HitTest(layout, {335, 72}).target,
            CandidateWindow::HitTarget::HealthBanner);
  // Above the banner the first row is a candidate even at its right edge.
  const auto hit = CandidateWindow::HitTest(layout, {399, 0});
  EXPECT_EQ(hit.target, CandidateWindow::HitTarget::Candidate);
  EXPECT_EQ(hit.index, 0);
}

TEST(CandidateWindowRenderTest, FillsEachRegionWithTheThemeAt96And144And192Dpi) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  const ScopedTestInputs inputs;
  struct ThemeCase {
    bool high_contrast;
    DWORD apps_use_light_theme;
  };
  for (const ThemeCase theme : {ThemeCase{false, 1u}, ThemeCase{false, 0u}, ThemeCase{true, 1u}}) {
    for (const UINT dpi : {96u, 144u, 192u}) {
      SCOPED_TRACE(::testing::Message() << "high_contrast=" << theme.high_contrast << " light="
                                        << theme.apps_use_light_theme << " dpi=" << dpi);
      tsf::testing::SetThemeInputsForTest(theme.high_contrast, theme.apps_use_light_theme);
      CandidateWindow::SetMonitorDpiForTest(dpi);
      CandidateWindow window;
      ASSERT_TRUE(window.Create());
      window.Show(POINT{20, 20}, {{L"候補", L"説明"}, {L"二", L""}}, 1, L"案内");
      window.ShowSecureToast();
      window.ShowHealthBanner(CandidateHealthState::DegradedModel);
      EXPECT_STREQ(window.failure_stage(), "");
      // The production path (RenderingEngine and the DComp surface) commits a frame.
      EXPECT_TRUE(window.RenderForTest()) << window.failure_stage();
      EXPECT_NE(GetWindowLongPtrW(window.hwnd_for_test(), GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP,
                0);

      std::vector<COLORREF> pixels;
      int width = 0;
      int height = 0;
      ASSERT_TRUE(window.RenderPixelsForTest(&pixels, &width, &height));
      const int row = MulDiv(24, static_cast<int>(dpi), 96);
      ASSERT_EQ(height, 7 * row);
      const auto at = [&](int x, int y) { return pixels[static_cast<size_t>(y * width + x)]; };
      const ThemeColors& colors = window.theme_for_test();
      // Inside the right padding no text reaches, so each row shows its fill.
      const int x = width - 3;
      EXPECT_EQ(at(x, row / 2), colors.background);
      EXPECT_EQ(at(x, row + row / 2), colors.selection);
      EXPECT_EQ(at(x, 2 * row + row / 2), colors.panel_background);
      EXPECT_EQ(at(x, 3 * row + row / 2), colors.info_background);
      EXPECT_EQ(at(x, 5 * row), colors.banner_background);
      EXPECT_EQ(at(0, row / 2), colors.border);
      EXPECT_EQ(at(width - 1, 6 * row), colors.border);
      window.Destroy();
    }
  }
}

// Pixels whose channels differ this much are colored, not gray.
bool RowHasColor(const std::vector<COLORREF>& pixels, int width, int top, int bottom) {
  for (int y = top; y < bottom; ++y) {
    for (int x = 1; x < width - 1; ++x) {
      const COLORREF pixel = pixels[static_cast<size_t>(y * width + x)];
      const int r = GetRValue(pixel);
      const int g = GetGValue(pixel);
      const int b = GetBValue(pixel);
      if (std::max({r, g, b}) - std::min({r, g, b}) > 64) return true;
    }
  }
  return false;
}

TEST(CandidateWindowRenderTest, DrawsEmojiWithTheColorFont) {
  Microsoft::WRL::ComPtr<IDWriteFactory> factory;
  ASSERT_TRUE(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                            reinterpret_cast<IUnknown**>(factory.GetAddressOf()))));
  Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts;
  ASSERT_TRUE(SUCCEEDED(factory->GetSystemFontCollection(&fonts)));
  UINT32 index = 0;
  BOOL exists = FALSE;
  ASSERT_TRUE(SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Emoji", &index, &exists)));
  if (!exists) GTEST_SKIP() << "Segoe UI Emoji is not installed";

  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  const ScopedTestInputs inputs;
  tsf::testing::SetThemeInputsForTest(false, 1u);
  CandidateWindow window;
  ASSERT_TRUE(window.Create());
  std::vector<COLORREF> pixels;
  int width = 0;
  int height = 0;
  const int row = window.current_metrics_for_test().item_height;

  // The second row is selected so the first row stays black text on white.
  window.Show(POINT{20, 20}, {{L"abc", L""}, {L"x", L""}}, 1);
  ASSERT_TRUE(window.RenderPixelsForTest(&pixels, &width, &height));
  EXPECT_FALSE(RowHasColor(pixels, width, 1, row));

  window.Show(POINT{20, 20}, {{L"😄", L""}, {L"x", L""}}, 1);
  ASSERT_TRUE(window.RenderPixelsForTest(&pixels, &width, &height));
  EXPECT_TRUE(RowHasColor(pixels, width, 1, row));
  window.Destroy();
}

TEST(CandidateWindowRenderTest, DpiChangeRemeasuresAtTheAnchorAndIgnoresTheSuggestedRect) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  const ScopedTestInputs inputs;
  CandidateWindow::SetMonitorDpiForTest(96);
  CandidateWindow window;
  ASSERT_TRUE(window.Create());
  window.Show(POINT{20, 20}, {{L"候補", L""}, {L"二", L""}}, 0);
  const HWND hwnd = window.hwnd_for_test();
  RECT bounds{};
  ASSERT_TRUE(GetWindowRect(hwnd, &bounds));
  EXPECT_EQ(bounds.bottom - bounds.top, 2 * 24);

  // The window moved to a 144 DPI monitor; the suggested 10x10 rect is not used.
  CandidateWindow::SetMonitorDpiForTest(144);
  RECT suggested{0, 0, 10, 10};
  SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&suggested));
  EXPECT_EQ(window.current_metrics_for_test().item_height, 36);
  ASSERT_TRUE(GetWindowRect(hwnd, &bounds));
  EXPECT_EQ(bounds.bottom - bounds.top, 2 * 36);
  EXPECT_GT(bounds.right - bounds.left, 10);
  EXPECT_STREQ(window.failure_stage(), "");
  EXPECT_TRUE(window.RenderForTest()) << window.failure_stage();
  window.Destroy();
}

TEST(CandidateWindowRenderTest, InitFailureKeepsTheWindowHiddenAndClicksDoNotSelect) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  const ScopedTestInputs inputs;
  using Failure = CandidateWindow::RenderFailureForTest;
  CandidateWindow::SetRenderFailureForTest(Failure::Initialize);
  CandidateWindow window;
  ASSERT_TRUE(window.Create());
  int clicks = 0;
  window.SetOnClick([&](int) { ++clicks; });
  window.Show(POINT{20, 20}, {{L"候補", L""}, {L"二", L""}}, 0);
  EXPECT_FALSE(window.IsVisible());
  EXPECT_STREQ(window.failure_stage(), "injected");
  EXPECT_EQ(window.render_init_failures_for_test(), 1);

  // A click on the first row does not select it.
  SendMessageW(window.hwnd_for_test(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, 2));
  EXPECT_EQ(clicks, 0);
  // Keys still move the selection; each change retries until the limit of three.
  window.MoveSelection(1);
  EXPECT_EQ(window.GetSelected(), 1);
  window.MoveSelection(1);
  EXPECT_EQ(window.render_init_failures_for_test(), 3);
  EXPECT_FALSE(window.IsVisible());

  // At the limit the device stack is not created again, even once it would work.
  CandidateWindow::SetRenderFailureForTest(Failure::None);
  window.MoveSelection(1);
  EXPECT_FALSE(window.IsVisible());
  EXPECT_EQ(window.render_init_failures_for_test(), 3);
  EXPECT_EQ(clicks, 0);
  window.Destroy();

  // A new Create starts counting again.
  ASSERT_TRUE(window.Create());
  window.Show(POINT{20, 20}, {{L"候補", L""}}, 0);
  EXPECT_TRUE(window.IsVisible());
  EXPECT_STREQ(window.failure_stage(), "");
  window.Destroy();
}

TEST(CandidateWindowRenderTest, RenderFailureHidesTheWindowUntilAStateChangeDrawsAgain) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  const ScopedTestInputs inputs;
  using Failure = CandidateWindow::RenderFailureForTest;
  CandidateWindow window;
  ASSERT_TRUE(window.Create());
  int clicks = 0;
  window.SetOnClick([&](int) { ++clicks; });

  // One failed Initialize, then the next state change draws and shows the window.
  CandidateWindow::SetRenderFailureForTest(Failure::Initialize);
  window.Show(POINT{20, 20}, {{L"候補", L""}, {L"二", L""}}, 0);
  EXPECT_FALSE(window.IsVisible());
  CandidateWindow::SetRenderFailureForTest(Failure::None);
  window.MoveSelection(1);
  EXPECT_TRUE(window.IsVisible());
  EXPECT_STREQ(window.failure_stage(), "");
  EXPECT_EQ(window.render_init_failures_for_test(), 0);

  // A failed frame while shown hides the window instead of keeping the old frame.
  CandidateWindow::SetRenderFailureForTest(Failure::Draw);
  window.MoveSelection(1);
  EXPECT_FALSE(window.IsVisible());
  SendMessageW(window.hwnd_for_test(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, 2));
  EXPECT_EQ(clicks, 0);
  EXPECT_EQ(window.GetSelected(), 0);

  CandidateWindow::SetRenderFailureForTest(Failure::None);
  window.MoveSelection(1);
  EXPECT_TRUE(window.IsVisible());
  EXPECT_EQ(window.GetSelected(), 1);
  // Hide clears the failed state; it is not shown again by a later change.
  window.Hide();
  window.MoveSelection(1);
  EXPECT_FALSE(window.IsVisible());
  window.Destroy();
}

}  // namespace
}  // namespace azookey::tsf
