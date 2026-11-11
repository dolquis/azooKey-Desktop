#include <dwrite.h>
#include <gtest/gtest.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>
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
    CandidateWindow::SetClientAreaAnimationForTest(-1);
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

TEST(CandidateWindowRenderTest, AppearingFadesInOnceAndHonorsTheAnimationSetting) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  const ScopedTestInputs inputs;
  using Appearance = CandidateWindow::AppearanceForTest;
  CandidateWindow::SetMonitorDpiForTest(96);
  CandidateWindow::SetClientAreaAnimationForTest(1);
  CandidateWindow window;
  ASSERT_TRUE(window.Create());
  EXPECT_EQ(window.last_appearance_for_test(), Appearance::None);

  window.Show(POINT{20, 20}, {{L"候補", L""}, {L"二", L""}}, 0);
  ASSERT_TRUE(window.IsVisible()) << window.failure_stage();
  EXPECT_EQ(window.last_appearance_for_test(), Appearance::Faded);
  EXPECT_TRUE(window.last_appearance_set_for_test());
  EXPECT_EQ(window.appearances_for_test(), 1);

  // A Show or a selection move of the visible window does not fade it again.
  window.Show(POINT{20, 20}, {{L"候補", L""}, {L"二", L""}, {L"三", L""}}, 1);
  window.MoveSelection(1);
  ASSERT_TRUE(window.IsVisible()) << window.failure_stage();
  EXPECT_EQ(window.appearances_for_test(), 1);

  // With client area animations off, the next appearance is at full opacity at once.
  window.Hide();
  CandidateWindow::SetClientAreaAnimationForTest(0);
  window.Show(POINT{20, 20}, {{L"候補", L""}}, 0);
  ASSERT_TRUE(window.IsVisible()) << window.failure_stage();
  EXPECT_EQ(window.last_appearance_for_test(), Appearance::Immediate);
  EXPECT_TRUE(window.last_appearance_set_for_test());
  EXPECT_EQ(window.appearances_for_test(), 2);

  // A frame that fails to draw keeps the window hidden and sets no opacity.
  window.Hide();
  CandidateWindow::SetRenderFailureForTest(CandidateWindow::RenderFailureForTest::Draw);
  window.Show(POINT{20, 20}, {{L"候補", L""}}, 0);
  EXPECT_FALSE(window.IsVisible());
  EXPECT_EQ(window.appearances_for_test(), 2);
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

TEST(CandidateWindowRenderTest, FooterChangesRetryFailedFramesWithoutReshowingHiddenUi) {
  const ScopedTestInputs inputs;
  using Failure = CandidateWindow::RenderFailureForTest;
  for (int change = 0; change < 7; ++change) {
    SCOPED_TRACE(change);
    CandidateWindow window;
    ASSERT_TRUE(window.Create());
    window.Show(POINT{20, 20}, {{L"候補", L""}}, 0);
    window.ShowSecureToast();
    window.ShowHealthBanner(CandidateHealthState::DegradedModel);
    CandidateWindow::SetRenderFailureForTest(Failure::Draw);
    window.MoveSelection(1);
    ASSERT_FALSE(window.IsVisible());
    if (change == 2) window.HideSecureToast();
    if (change == 5) window.HideHealthBanner();
    CandidateWindow::SetRenderFailureForTest(Failure::None);
    const auto apply_change = [&] {
      switch (change) {
        case 0:
          window.SetSecureIndicator(true);
          break;
        case 1:
          window.ShowSecureToast();
          break;
        case 2:
          window.ResumeSecureToast();
          break;
        case 3:
          window.ShowHealthBanner(CandidateHealthState::SafeMode);
          break;
        case 4:
          window.HideHealthBanner();
          break;
        case 5:
          window.ResumeHealthBanner();
          break;
        case 6:
          window.HideSecureToast();
          break;
      }
    };
    apply_change();
    EXPECT_TRUE(window.IsVisible());
    EXPECT_STREQ(window.failure_stage(), "");
    window.Hide();
    apply_change();
    EXPECT_FALSE(window.IsVisible());
  }
}

TEST(CandidateWindowRenderTest, DetailsFailureReturnsFalseAndClosesOnlyThePopup) {
  const ScopedTestInputs inputs;
  using Failure = CandidateWindow::RenderFailureForTest;
  CandidateWindow window;
  ASSERT_TRUE(window.Create());
  window.Show(POINT{20, 20}, {{L"候補", L""}}, 0);
  window.ShowHealthBanner(CandidateHealthState::DegradedModel);
  ASSERT_TRUE(window.IsVisible());

  CandidateWindow::SetRenderFailureForTest(Failure::Initialize);
  EXPECT_FALSE(window.ShowDetailsForTest());
  EXPECT_FALSE(window.details_visible_for_test());
  EXPECT_TRUE(window.IsVisible());
  EXPECT_STREQ(window.failure_stage(), "");
  EXPECT_EQ(window.failure_hr(), S_OK);

  CandidateWindow::SetRenderFailureForTest(Failure::None);
  ASSERT_TRUE(window.ShowDetailsForTest());
  ASSERT_TRUE(window.details_visible_for_test());
  CandidateWindow::SetRenderFailureForTest(Failure::Draw);
  EXPECT_FALSE(window.RenderDetailsForTest());
  EXPECT_FALSE(window.details_visible_for_test());
  EXPECT_TRUE(window.IsVisible());
  EXPECT_STREQ(window.failure_stage(), "");
  EXPECT_EQ(window.failure_hr(), S_OK);
}

// A composition's text extent: the window opens below its bottom-left.
constexpr RECT kAnchor{20, 4, 120, 20};

const std::vector<std::wstring> kSixPredictions = {L"予測一", L"予測二", L"予測三",
                                                   L"予測四", L"予測五", L"予測六"};

TEST(CandidateWindowPredictionTest, RowsAreTheOnlyClickRegionsAt96And144And192Dpi) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  const ScopedTestInputs inputs;
  using Target = CandidateWindow::HitTarget;
  for (const UINT dpi : {96u, 144u, 192u}) {
    SCOPED_TRACE(dpi);
    CandidateWindow::SetMonitorDpiForTest(dpi);
    CandidateWindow window;
    ASSERT_TRUE(window.Create());
    int candidate_clicks = 0;
    std::vector<int> prediction_clicks;
    window.SetOnClick([&](int) { ++candidate_clicks; });
    window.SetOnPredictionClick([&](int index) { prediction_clicks.push_back(index); });
    // The lock and the banner belong to candidate mode only.
    window.SetSecureIndicator(true);
    window.ShowHealthBanner(CandidateHealthState::DegradedModel);
    window.ShowPredictions(kAnchor, kSixPredictions);
    ASSERT_TRUE(window.IsShowingPredictions()) << window.failure_stage();
    EXPECT_EQ(window.mode(), CandidateWindow::Mode::Prediction);
    EXPECT_EQ(window.GetPredictionCount(), CandidateWindow::kMaxPredictions);
    EXPECT_EQ(window.GetCount(), 0);

    const int row = MulDiv(24, static_cast<int>(dpi), 96);
    RECT client{};
    ASSERT_TRUE(GetClientRect(window.hwnd_for_test(), &client));
    // Five rows and no margin above or below them.
    EXPECT_EQ(client.bottom, 5 * row);
    RECT bounds{};
    ASSERT_TRUE(GetWindowRect(window.hwnd_for_test(), &bounds));
    EXPECT_EQ(bounds.left, kAnchor.left);
    EXPECT_EQ(bounds.top, kAnchor.bottom);
    const int width = client.right;
    const auto expect = [&](POINT point, Target target, int index = -1) {
      const auto hit = window.HitTestForTest(point);
      EXPECT_EQ(hit.target, target) << point.x << "," << point.y;
      EXPECT_EQ(hit.index, index) << point.x << "," << point.y;
    };
    expect({0, 0}, Target::Prediction, 0);
    expect({width - 1, 0}, Target::Prediction, 0);
    expect({width - 1, row - 1}, Target::Prediction, 0);
    expect({0, row}, Target::Prediction, 1);
    expect({width - 1, 5 * row - 1}, Target::Prediction, 4);
    expect({0, 5 * row}, Target::None);

    // A press accepts the prediction; the candidate callback never sees it.
    SendMessageW(window.hwnd_for_test(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, row + 2));
    EXPECT_EQ(prediction_clicks, std::vector<int>{1});
    EXPECT_EQ(candidate_clicks, 0);

    // Candidate mode replaces the predictions; the two are never drawn together.
    window.Show(POINT{20, 20}, {{L"候補", L""}}, 0);
    EXPECT_EQ(window.mode(), CandidateWindow::Mode::Candidate);
    EXPECT_FALSE(window.IsShowingPredictions());
    EXPECT_EQ(window.GetPredictionCount(), 0);
    expect({0, 0}, Target::Candidate, 0);
    window.Destroy();
  }
}

TEST(CandidateWindowPredictionTest, DrawsKeyHintsWithoutNumbersOrSelection) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  const ScopedTestInputs inputs;
  for (const UINT dpi : {96u, 144u, 192u}) {
    SCOPED_TRACE(dpi);
    tsf::testing::SetThemeInputsForTest(false, 1u);
    CandidateWindow::SetMonitorDpiForTest(dpi);
    CandidateWindow window;
    ASSERT_TRUE(window.Create());
    window.ShowSecureToast();
    window.ShowPredictions(kAnchor, {L"漢字漢字", L"漢字漢字", L"漢字漢字"});
    ASSERT_TRUE(window.IsShowingPredictions()) << window.failure_stage();
    EXPECT_TRUE(window.RenderForTest()) << window.failure_stage();

    std::vector<COLORREF> pixels;
    int width = 0;
    int height = 0;
    ASSERT_TRUE(window.RenderPixelsForTest(&pixels, &width, &height));
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const int row = scale(24);
    // No toast row: three rows only.
    ASSERT_EQ(height, 3 * row);
    const ThemeColors& colors = window.theme_for_test();
    const auto at = [&](int x, int y) { return pixels[static_cast<size_t>(y * width + x)]; };
    // The start of the hint column: Tab, ⇧Tab, then empty.
    const auto hint_has_ink = [&](int index) {
      for (int y = index * row + 2; y < (index + 1) * row - 2; ++y)
        for (int x = scale(8); x < scale(8) + scale(10); ++x)
          if (at(x, y) != colors.background) return true;
      return false;
    };
    EXPECT_TRUE(hint_has_ink(0));
    EXPECT_TRUE(hint_has_ink(1));
    EXPECT_FALSE(hint_has_ink(2));
    // Hints are sub_text: grayscale coverage of that gray over white is never
    // darker than it, while the prediction text reaches the darker text color.
    const auto darkest = [&](int index, int left, int right) {
      int value = 255;
      for (int y = index * row + 1; y < (index + 1) * row - 1; ++y)
        for (int x = left; x < right; ++x) value = std::min(value, int{GetRValue(at(x, y))});
      return value;
    };
    const int sub_text = GetRValue(colors.sub_text);
    ASSERT_GT(sub_text, GetRValue(colors.text) + 40);
    for (int i = 0; i < 2; ++i) {
      SCOPED_TRACE(i);
      const int hint = darkest(i, scale(8), scale(8) + scale(14));
      EXPECT_GE(hint, sub_text - 8);
      EXPECT_LT(hint, 200);
      EXPECT_LT(darkest(i, scale(60), width - scale(8)), sub_text - 30);
    }
    // No row is highlighted as selected.
    for (int i = 0; i < 3; ++i) EXPECT_EQ(at(width - 3, i * row + row / 2), colors.background) << i;
    EXPECT_EQ(at(0, row / 2), colors.border);
    window.Destroy();
  }
}

TEST(CandidateWindowPredictionTest, RenderFailureKeepsPredictionsHiddenAndUnclickable) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  const ScopedTestInputs inputs;
  using Failure = CandidateWindow::RenderFailureForTest;
  CandidateWindow window;
  ASSERT_TRUE(window.Create());
  int clicks = 0;
  window.SetOnPredictionClick([&](int) { ++clicks; });

  CandidateWindow::SetRenderFailureForTest(Failure::Draw);
  window.ShowPredictions(kAnchor, {L"一", L"二"});
  EXPECT_FALSE(window.IsVisible());
  EXPECT_FALSE(window.IsShowingPredictions());
  EXPECT_STREQ(window.failure_stage(), "injected");
  SendMessageW(window.hwnd_for_test(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, 2));
  EXPECT_EQ(clicks, 0);

  // The next Show draws and shows the rows again.
  CandidateWindow::SetRenderFailureForTest(Failure::None);
  window.ShowPredictions(kAnchor, {L"一", L"二"});
  EXPECT_TRUE(window.IsShowingPredictions()) << window.failure_stage();
  SendMessageW(window.hwnd_for_test(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, 2));
  EXPECT_EQ(clicks, 1);

  // A failed frame while shown hides the rows; the next theme change redraws
  // the same predictions at the same anchor and shows them again.
  CandidateWindow::SetRenderFailureForTest(Failure::Draw);
  SendMessageW(window.hwnd_for_test(), WM_THEMECHANGED, 0, 0);
  EXPECT_FALSE(window.IsShowingPredictions());
  CandidateWindow::SetRenderFailureForTest(Failure::None);
  SendMessageW(window.hwnd_for_test(), WM_THEMECHANGED, 0, 0);
  EXPECT_TRUE(window.IsShowingPredictions()) << window.failure_stage();
  EXPECT_EQ(window.GetPredictionCount(), 2);
  // Empty predictions hide the window in prediction mode only.
  window.ShowPredictions(kAnchor, {});
  EXPECT_FALSE(window.IsVisible());
  window.Show(POINT{20, 20}, {{L"候補", L""}}, 0);
  window.ShowPredictions(kAnchor, {});
  EXPECT_TRUE(window.IsVisible());
  EXPECT_EQ(window.mode(), CandidateWindow::Mode::Candidate);
  window.Destroy();
}

TEST(CandidateWindowPredictionTest, BothModesPickTheMonitorAtTheAnchorsBottomLeft) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  const ScopedTestInputs inputs;
  CandidateWindow window;
  ASSERT_TRUE(window.Create());
  // An extent across the edge of two monitors side by side at x = 1920: its center
  // is on the right one, its bottom-left on the left one (legacy-parity-spec §9.2).
  constexpr RECT kAcrossTheEdge{1900, 100, 2100, 120};
  window.ShowPredictions(kAcrossTheEdge, {L"一"});
  const POINT prediction = CandidateWindow::LastMonitorPointForTest();
  EXPECT_EQ(prediction.x, kAcrossTheEdge.left);
  EXPECT_EQ(prediction.y, kAcrossTheEdge.bottom);
  // The candidate mode is shown at the same anchor point and picks the same one.
  window.Show(POINT{kAcrossTheEdge.left, kAcrossTheEdge.bottom}, {{L"候補", L""}}, 0);
  const POINT candidate = CandidateWindow::LastMonitorPointForTest();
  EXPECT_EQ(candidate.x, prediction.x);
  EXPECT_EQ(candidate.y, prediction.y);
  window.Destroy();
}

TEST(CandidateWindowPredictionTest, OpensAboveTheAnchorAndRemeasuresOnDpiChange) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  const ScopedTestInputs inputs;
  CandidateWindow::SetMonitorDpiForTest(96);
  CandidateWindow window;
  ASSERT_TRUE(window.Create());
  // An anchor at the bottom of the primary work area: the window goes above it.
  RECT work{};
  ASSERT_TRUE(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0));
  const RECT anchor{work.left + 20, work.bottom - 30, work.left + 120, work.bottom - 10};
  window.ShowPredictions(anchor, {L"一", L"二"});
  ASSERT_TRUE(window.IsShowingPredictions()) << window.failure_stage();
  const HWND hwnd = window.hwnd_for_test();
  RECT bounds{};
  ASSERT_TRUE(GetWindowRect(hwnd, &bounds));
  EXPECT_EQ(bounds.bottom, anchor.top);
  EXPECT_EQ(bounds.bottom - bounds.top, 2 * 24);

  // At 144 DPI the suggested rect is ignored and the rows are measured again.
  CandidateWindow::SetMonitorDpiForTest(144);
  RECT suggested{0, 0, 10, 10};
  SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&suggested));
  ASSERT_TRUE(GetWindowRect(hwnd, &bounds));
  EXPECT_EQ(bounds.bottom - bounds.top, 2 * 36);
  EXPECT_EQ(bounds.bottom, anchor.top);
  EXPECT_TRUE(window.IsShowingPredictions());
  window.Destroy();
}

TEST(CandidateWindowScrollTest, VisibleRowsFitTheWorkAreaWithAPositionRow) {
  // Every row fits, with or without the elements below them.
  EXPECT_EQ(CandidateWindow::ComputeVisibleRows(5, 24, 0, 120), 5);
  EXPECT_EQ(CandidateWindow::ComputeVisibleRows(4, 24, 24, 120), 4);
  // One row short: the position row takes a row too, so two rows go.
  EXPECT_EQ(CandidateWindow::ComputeVisibleRows(6, 24, 0, 120), 4);
  // The notice, toast and banner keep their height; the rows give way.
  EXPECT_EQ(CandidateWindow::ComputeVisibleRows(32, 24, 24 * 4, 240), 5);
  // At least one candidate row stays, even when nothing else fits.
  EXPECT_EQ(CandidateWindow::ComputeVisibleRows(32, 24, 24 * 20, 240), 1);
  // No work area or no candidates.
  EXPECT_EQ(CandidateWindow::ComputeVisibleRows(32, 24, 0, 0), 32);
  EXPECT_EQ(CandidateWindow::ComputeVisibleRows(0, 24, 0, 240), 0);
}

TEST(CandidateWindowScrollTest, FirstVisibleRowFollowsTheSelectionAndTheWrap) {
  constexpr int kCount = 12;
  constexpr int kRows = 4;
  // Everything fits: always from the top.
  EXPECT_EQ(CandidateWindow::ComputeFirstVisibleRow(3, 7, 4, 4), 0);
  // Inside the range: unchanged.
  EXPECT_EQ(CandidateWindow::ComputeFirstVisibleRow(0, 3, kCount, kRows), 0);
  EXPECT_EQ(CandidateWindow::ComputeFirstVisibleRow(5, 6, kCount, kRows), 5);
  // One past the bottom edge: the range moves down by one.
  EXPECT_EQ(CandidateWindow::ComputeFirstVisibleRow(0, 4, kCount, kRows), 1);
  // One past the top edge: the range moves up by one.
  EXPECT_EQ(CandidateWindow::ComputeFirstVisibleRow(5, 4, kCount, kRows), 4);
  // The wrap from the first to the last candidate shows the last range, and back.
  EXPECT_EQ(CandidateWindow::ComputeFirstVisibleRow(0, kCount - 1, kCount, kRows), kCount - kRows);
  EXPECT_EQ(CandidateWindow::ComputeFirstVisibleRow(kCount - kRows, 0, kCount, kRows), 0);
  // A stale range from a longer list is pulled back inside the list.
  EXPECT_EQ(CandidateWindow::ComputeFirstVisibleRow(20, 11, kCount, kRows), kCount - kRows);
}

TEST(CandidateWindowScrollTest, HitTestMapsDrawnRowsToTheFullList) {
  using Target = CandidateWindow::HitTarget;
  for (const int row : {24, 36, 48}) {
    SCOPED_TRACE(row);
    CandidateWindow::HitLayout layout{row, 3, 200, 0, false, 5 * row, {}, false, {}};
    layout.first_index = 7;
    EXPECT_EQ(CandidateWindow::HitTest(layout, {0, 0}).index, 7);
    EXPECT_EQ(CandidateWindow::HitTest(layout, {199, 3 * row - 1}).index, 9);
    // The position row below the drawn rows is not a candidate.
    EXPECT_EQ(CandidateWindow::HitTest(layout, {0, 3 * row}).target, Target::None);
    EXPECT_EQ(CandidateWindow::HitTest(layout, {0, -1}).target, Target::None);
  }
}

TEST(CandidateWindowScrollTest, ManyCandidatesStayInsideTheWorkAreaAndScroll) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  const ScopedTestInputs inputs;
  tsf::testing::SetThemeInputsForTest(false, 1u);
  RECT work{};
  ASSERT_TRUE(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0));
  const int work_height = work.bottom - work.top;
  for (const UINT dpi : {96u, 144u, 192u}) {
    SCOPED_TRACE(dpi);
    CandidateWindow::SetMonitorDpiForTest(dpi);
    const int row = MulDiv(24, static_cast<int>(dpi), 96);
    // More candidates than any work area holds.
    const int count = work_height / row + 10;
    std::vector<CandidateViewItem> items;
    for (int i = 0; i < count; ++i) items.push_back({L"候補" + std::to_wstring(i), L""});
    CandidateWindow window;
    ASSERT_TRUE(window.Create());
    int clicked = -1;
    window.SetOnClick([&](int index) { clicked = index; });
    window.Show(POINT{work.left + 20, work.top + 20}, items, 0);
    ASSERT_TRUE(window.IsVisible()) << window.failure_stage();

    const int visible = window.GetVisibleRowCount();
    EXPECT_EQ(visible, (work_height - row) / row);
    EXPECT_EQ(window.GetFirstVisibleRow(), 0);
    RECT bounds{};
    ASSERT_TRUE(GetWindowRect(window.hwnd_for_test(), &bounds));
    EXPECT_EQ(bounds.bottom - bounds.top, (visible + 1) * row);
    EXPECT_GE(bounds.top, work.top);
    EXPECT_LE(bounds.bottom, work.bottom);
    EXPECT_EQ(window.footer_top_for_test(), (visible + 1) * row);

    // The wrap to the last candidate shows the last range; numbers stay global.
    window.MoveSelection(-1);
    EXPECT_EQ(window.GetSelected(), count - 1);
    EXPECT_EQ(window.GetFirstVisibleRow(), count - visible);
    EXPECT_EQ(window.GetCount(), count);
    const auto top = window.HitTestForTest({2, 2});
    EXPECT_EQ(top.target, CandidateWindow::HitTarget::Candidate);
    EXPECT_EQ(top.index, count - visible);
    SendMessageW(window.hwnd_for_test(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, row + 2));
    EXPECT_EQ(clicked, count - visible + 1);
    EXPECT_EQ(window.GetSelected(), count - visible + 1);
    EXPECT_EQ(window.GetFirstVisibleRow(), count - visible);

    // The position row is drawn in sub_text below the rows, and not filled.
    std::vector<COLORREF> pixels;
    int width = 0;
    int height = 0;
    ASSERT_TRUE(window.RenderPixelsForTest(&pixels, &width, &height));
    const ThemeColors& colors = window.theme_for_test();
    const auto at = [&](int x, int y) { return pixels[static_cast<size_t>(y * width + x)]; };
    int darkest = 255;
    for (int y = visible * row + 1; y < (visible + 1) * row - 1; ++y)
      for (int x = 1; x < width - 1; ++x) darkest = std::min(darkest, int{GetRValue(at(x, y))});
    EXPECT_LT(darkest, 200);
    EXPECT_GE(darkest, GetRValue(colors.sub_text) - 8);
    EXPECT_EQ(at(width - 3, visible * row + row / 2), colors.background);

    // Back to the first candidate: the first range again.
    window.SetSelected(0);
    EXPECT_EQ(window.GetFirstVisibleRow(), 0);
    window.Destroy();
  }
}

TEST(CandidateWindowScrollTest, FewCandidatesHaveNoPositionRow) {
  ScopedPerMonitorDpi dpi_context;
  ASSERT_TRUE(dpi_context.valid());
  const ScopedTestInputs inputs;
  CandidateWindow::SetMonitorDpiForTest(96);
  CandidateWindow window;
  ASSERT_TRUE(window.Create());
  window.Show(POINT{20, 20}, {{L"一", L""}, {L"二", L""}, {L"三", L""}}, 2, L"案内");
  EXPECT_EQ(window.GetVisibleRowCount(), 3);
  EXPECT_EQ(window.GetFirstVisibleRow(), 0);
  // Rows, then the notice row; nothing in between.
  EXPECT_EQ(window.footer_top_for_test(), 4 * 24);
  window.Destroy();
}

}  // namespace
}  // namespace azookey::tsf
