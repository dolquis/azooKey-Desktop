#include <gtest/gtest.h>

#include <cstdint>

#include "azookey/tsf/CaretRectResolver.h"
#include "azookey/tsf/TextService.h"

namespace {

int g_gui_thread_info_calls = 0;
int g_client_to_screen_calls = 0;
int g_physical_cursor_pos_calls = 0;
int g_logical_to_physical_calls = 0;
int g_monitor_scale_percent_calls = 0;
RECT g_gui_caret_rect{};
bool g_gui_thread_info_succeeds = false;
bool g_client_to_screen_succeeds = false;
bool g_physical_cursor_pos_succeeds = false;
bool g_logical_to_physical_succeeds = false;
POINT g_screen_offset{};
POINT g_physical_cursor_point{};
POINT g_physical_offset{};
HWND g_expected_transform_window = nullptr;
UINT g_monitor_scale_percent = 100;

BOOL WINAPI FakeGetGuiThreadInfo(DWORD thread_id, PGUITHREADINFO info) {
  ++g_gui_thread_info_calls;
  EXPECT_EQ(thread_id, 0u);
  if (!g_gui_thread_info_succeeds) return FALSE;
  info->hwndCaret = reinterpret_cast<HWND>(static_cast<uintptr_t>(1));
  info->rcCaret = g_gui_caret_rect;
  return TRUE;
}

BOOL WINAPI FakeClientToScreen(HWND hwnd, LPPOINT point) {
  ++g_client_to_screen_calls;
  EXPECT_EQ(hwnd, reinterpret_cast<HWND>(static_cast<uintptr_t>(1)));
  if (!g_client_to_screen_succeeds) return FALSE;
  point->x += g_screen_offset.x;
  point->y += g_screen_offset.y;
  return TRUE;
}

BOOL WINAPI FakeGetPhysicalCursorPos(LPPOINT point) {
  ++g_physical_cursor_pos_calls;
  if (!g_physical_cursor_pos_succeeds) return FALSE;
  *point = g_physical_cursor_point;
  return TRUE;
}

BOOL WINAPI FakeLogicalToPhysicalPointForPerMonitorDpi(HWND hwnd, LPPOINT point) {
  ++g_logical_to_physical_calls;
  EXPECT_EQ(hwnd, g_expected_transform_window);
  if (!g_logical_to_physical_succeeds) return FALSE;
  point->x += g_physical_offset.x;
  point->y += g_physical_offset.y;
  return TRUE;
}

UINT FakeGetMonitorScalePercent(POINT screen_point) {
  ++g_monitor_scale_percent_calls;
  EXPECT_EQ(screen_point.x, g_physical_cursor_point.x);
  EXPECT_EQ(screen_point.y, g_physical_cursor_point.y);
  return g_monitor_scale_percent;
}

class CaretPositionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_gui_thread_info_calls = 0;
    g_client_to_screen_calls = 0;
    g_physical_cursor_pos_calls = 0;
    g_logical_to_physical_calls = 0;
    g_monitor_scale_percent_calls = 0;
    g_gui_caret_rect = {};
    g_gui_thread_info_succeeds = false;
    g_client_to_screen_succeeds = false;
    g_physical_cursor_pos_succeeds = false;
    g_logical_to_physical_succeeds = false;
    g_screen_offset = {};
    g_physical_cursor_point = {};
    g_physical_offset = {};
    g_expected_transform_window = nullptr;
    g_monitor_scale_percent = 100;
    azookey::tsf::testing::SetCaretWin32ApiForTest(
        &FakeGetGuiThreadInfo, &FakeClientToScreen, &FakeGetPhysicalCursorPos,
        &FakeLogicalToPhysicalPointForPerMonitorDpi, &FakeGetMonitorScalePercent);
  }

  void TearDown() override { azookey::tsf::testing::ClearCaretWin32ApiForTest(); }
};

TEST_F(CaretPositionTest, UsesNonEmptyTextExtentWithoutFallback) {
  const RECT text_extent{10, 20, 30, 44};

  const auto anchor = azookey::tsf::testing::ResolveCaretAnchorForTest(&text_extent);

  EXPECT_TRUE(anchor.valid);
  EXPECT_EQ(anchor.point.x, 10);
  EXPECT_EQ(anchor.point.y, 44);
  EXPECT_EQ(g_gui_thread_info_calls, 0);
  EXPECT_EQ(g_client_to_screen_calls, 0);
  EXPECT_EQ(g_physical_cursor_pos_calls, 0);
  EXPECT_EQ(g_logical_to_physical_calls, 0);
}

TEST_F(CaretPositionTest, NormalizesTextExtentToPhysicalCoordinatesUsingDocumentWindow) {
  const RECT text_extent{10, 20, 30, 44};
  g_expected_transform_window = reinterpret_cast<HWND>(static_cast<uintptr_t>(2));
  g_logical_to_physical_succeeds = true;
  g_physical_offset = {5, 22};

  const auto anchor =
      azookey::tsf::testing::ResolveCaretAnchorForTest(&text_extent, g_expected_transform_window);

  EXPECT_TRUE(anchor.valid);
  EXPECT_EQ(anchor.point.x, 15);
  EXPECT_EQ(anchor.point.y, 66);
  EXPECT_EQ(g_logical_to_physical_calls, 1);
  EXPECT_EQ(g_gui_thread_info_calls, 0);
  EXPECT_EQ(g_client_to_screen_calls, 0);
  EXPECT_EQ(g_physical_cursor_pos_calls, 0);
}

TEST_F(CaretPositionTest, KeepsTextExtentWhenPhysicalCoordinateConversionFails) {
  const RECT text_extent{10, 20, 30, 44};
  g_expected_transform_window = reinterpret_cast<HWND>(static_cast<uintptr_t>(2));

  const auto anchor =
      azookey::tsf::testing::ResolveCaretAnchorForTest(&text_extent, g_expected_transform_window);

  EXPECT_TRUE(anchor.valid);
  EXPECT_EQ(anchor.point.x, 10);
  EXPECT_EQ(anchor.point.y, 44);
  EXPECT_EQ(g_logical_to_physical_calls, 1);
  EXPECT_EQ(g_gui_thread_info_calls, 0);
  EXPECT_EQ(g_client_to_screen_calls, 0);
  EXPECT_EQ(g_physical_cursor_pos_calls, 0);
}

TEST_F(CaretPositionTest, NormalizesGuiThreadCaretToPhysicalCoordinatesUsingCaretWindow) {
  const RECT empty_text_extent{10, 20, 10, 44};
  g_gui_thread_info_succeeds = true;
  g_client_to_screen_succeeds = true;
  g_gui_caret_rect = {4, 5, 6, 25};
  g_screen_offset = {100, 200};
  g_expected_transform_window = reinterpret_cast<HWND>(static_cast<uintptr_t>(1));
  g_logical_to_physical_succeeds = true;
  g_physical_offset = {25, 50};

  const auto anchor = azookey::tsf::testing::ResolveCaretAnchorForTest(&empty_text_extent);

  EXPECT_TRUE(anchor.valid);
  EXPECT_EQ(anchor.point.x, 129);
  EXPECT_EQ(anchor.point.y, 275);
  EXPECT_EQ(g_gui_thread_info_calls, 1);
  EXPECT_EQ(g_client_to_screen_calls, 1);
  EXPECT_EQ(g_physical_cursor_pos_calls, 0);
  EXPECT_EQ(g_logical_to_physical_calls, 1);
}

TEST_F(CaretPositionTest, KeepsGuiThreadScreenCoordinatesWhenPhysicalConversionFails) {
  g_gui_thread_info_succeeds = true;
  g_client_to_screen_succeeds = true;
  g_gui_caret_rect = {4, 5, 6, 25};
  g_screen_offset = {100, 200};
  g_expected_transform_window = reinterpret_cast<HWND>(static_cast<uintptr_t>(1));
  g_logical_to_physical_succeeds = false;

  const auto anchor = azookey::tsf::testing::ResolveCaretAnchorForTest(nullptr);

  EXPECT_TRUE(anchor.valid);
  EXPECT_EQ(anchor.point.x, 104);
  EXPECT_EQ(anchor.point.y, 225);
  EXPECT_EQ(g_gui_thread_info_calls, 1);
  EXPECT_EQ(g_client_to_screen_calls, 1);
  EXPECT_EQ(g_physical_cursor_pos_calls, 0);
  EXPECT_EQ(g_logical_to_physical_calls, 1);
}

TEST_F(CaretPositionTest, ZeroGuiCaretFallsBackToScaledPhysicalCursorPosition) {
  g_gui_thread_info_succeeds = true;
  g_physical_cursor_pos_succeeds = true;
  g_physical_cursor_point = {321, 654};
  g_monitor_scale_percent = 150;

  const auto anchor = azookey::tsf::testing::ResolveCaretAnchorForTest(nullptr);

  EXPECT_TRUE(anchor.valid);
  EXPECT_EQ(anchor.point.x, 321);
  EXPECT_EQ(anchor.point.y, 678);
  EXPECT_EQ(g_gui_thread_info_calls, 1);
  EXPECT_EQ(g_client_to_screen_calls, 0);
  EXPECT_EQ(g_physical_cursor_pos_calls, 1);
  EXPECT_EQ(g_monitor_scale_percent_calls, 1);
}

TEST_F(CaretPositionTest, PhysicalCursorUsesDefaultHeightWhenMonitorScaleIsUnavailable) {
  g_physical_cursor_pos_succeeds = true;
  g_physical_cursor_point = {321, 654};
  g_monitor_scale_percent = 0;

  const auto anchor = azookey::tsf::testing::ResolveCaretAnchorForTest(nullptr);

  EXPECT_TRUE(anchor.valid);
  EXPECT_EQ(anchor.point.x, 321);
  EXPECT_EQ(anchor.point.y, 670);
  EXPECT_EQ(g_monitor_scale_percent_calls, 1);
}

TEST_F(CaretPositionTest, AllFailuresReturnInvalidOrigin) {
  const auto anchor = azookey::tsf::testing::ResolveCaretAnchorForTest(nullptr);

  EXPECT_FALSE(anchor.valid);
  EXPECT_EQ(anchor.point.x, 0);
  EXPECT_EQ(anchor.point.y, 0);
}

azookey::tsf::CaretWin32Api FakeCaretApi() {
  return {&FakeGetGuiThreadInfo, &FakeClientToScreen, &FakeGetPhysicalCursorPos,
          &FakeLogicalToPhysicalPointForPerMonitorDpi, &FakeGetMonitorScalePercent};
}

TEST_F(CaretPositionTest, CaretRectConvertsBothCornersWithTheDocumentWindow) {
  const RECT text_extent{10, 20, 30, 44};
  g_expected_transform_window = reinterpret_cast<HWND>(static_cast<uintptr_t>(2));
  g_logical_to_physical_succeeds = true;
  g_physical_offset = {5, 22};
  RECT physical{};

  EXPECT_TRUE(azookey::tsf::ResolveCaretRect(FakeCaretApi(), text_extent,
                                             g_expected_transform_window, &physical));

  EXPECT_EQ(physical.left, 15);
  EXPECT_EQ(physical.top, 42);
  EXPECT_EQ(physical.right, 35);
  EXPECT_EQ(physical.bottom, 66);
  EXPECT_EQ(g_logical_to_physical_calls, 2);
}

TEST_F(CaretPositionTest, CaretRectIsUnavailableWhenPhysicalConversionFails) {
  const RECT text_extent{10, 20, 30, 44};
  g_expected_transform_window = reinterpret_cast<HWND>(static_cast<uintptr_t>(2));
  RECT physical{1, 2, 3, 4};

  EXPECT_FALSE(azookey::tsf::ResolveCaretRect(FakeCaretApi(), text_extent,
                                              g_expected_transform_window, &physical));
  EXPECT_EQ(physical.left, 1);
}

TEST_F(CaretPositionTest, CaretRectKeepsTextExtentWithoutWindowAndRejectsEmptyExtent) {
  RECT physical{};

  EXPECT_TRUE(
      azookey::tsf::ResolveCaretRect(FakeCaretApi(), RECT{10, 20, 30, 44}, nullptr, &physical));
  EXPECT_EQ(physical.left, 10);
  EXPECT_EQ(physical.bottom, 44);
  EXPECT_FALSE(
      azookey::tsf::ResolveCaretRect(FakeCaretApi(), RECT{10, 20, 10, 44}, nullptr, &physical));
  EXPECT_EQ(g_logical_to_physical_calls, 0);
}

TEST_F(CaretPositionTest, AnchorRectUsesTheMonitorScaledFallbackHeight) {
  g_physical_cursor_point = {100, 200};
  g_monitor_scale_percent = 150;

  const RECT rect = azookey::tsf::CaretRectFromAnchor(FakeCaretApi(), {100, 200});

  EXPECT_EQ(rect.left, 100);
  EXPECT_EQ(rect.top, 176);
  EXPECT_EQ(rect.right, 101);
  EXPECT_EQ(rect.bottom, 200);
}

int g_monitor_info_calls = 0;
bool g_monitor_info_succeeds = true;

// Two side-by-side monitors: the secondary one sits left of the primary at
// negative coordinates and has a taskbar on its top edge.
HMONITOR WINAPI FakeMonitorFromPoint(POINT point, DWORD flags) {
  EXPECT_EQ(flags, static_cast<DWORD>(MONITOR_DEFAULTTONEAREST));
  return reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(point.x < 0 ? 2 : 1));
}

BOOL WINAPI FakeGetMonitorInfo(HMONITOR monitor, LPMONITORINFO info) {
  ++g_monitor_info_calls;
  if (!g_monitor_info_succeeds) return FALSE;
  info->rcWork = monitor == reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(2))
                     ? RECT{-1280, 40, 0, 1024}
                     : RECT{0, 0, 1920, 1040};
  return TRUE;
}

int WINAPI FakeGetSystemMetrics(int index) {
  switch (index) {
    case SM_XVIRTUALSCREEN:
      return -1280;
    case SM_YVIRTUALSCREEN:
      return 0;
    case SM_CXVIRTUALSCREEN:
      return 3200;
    case SM_CYVIRTUALSCREEN:
      return 1080;
    default:
      return 0;
  }
}

class MonitorWorkAreaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_monitor_info_calls = 0;
    g_monitor_info_succeeds = true;
  }

  static azookey::tsf::MonitorWin32Api Api() {
    return {&FakeMonitorFromPoint, &FakeGetMonitorInfo, &FakeGetSystemMetrics};
  }
};

TEST_F(MonitorWorkAreaTest, PicksTheWorkAreaOfTheMonitorUnderThePoint) {
  const auto primary = azookey::tsf::ResolveMonitorWorkArea(Api(), {500, 500});
  const auto secondary = azookey::tsf::ResolveMonitorWorkArea(Api(), {-10, 500});

  EXPECT_EQ(primary.work_area.right, 1920);
  EXPECT_EQ(primary.work_area.bottom, 1040);
  EXPECT_EQ(secondary.work_area.left, -1280);
  EXPECT_EQ(secondary.work_area.top, 40);
  EXPECT_NE(primary.monitor, secondary.monitor);
}

TEST_F(MonitorWorkAreaTest, FallsBackToTheVirtualScreenWhenMonitorInfoFails) {
  g_monitor_info_succeeds = false;

  const auto result = azookey::tsf::ResolveMonitorWorkArea(Api(), {500, 500});

  EXPECT_EQ(g_monitor_info_calls, 1);
  EXPECT_EQ(result.work_area.left, -1280);
  EXPECT_EQ(result.work_area.top, 0);
  EXPECT_EQ(result.work_area.right, 1920);
  EXPECT_EQ(result.work_area.bottom, 1080);
}

TEST_F(MonitorWorkAreaTest, ReturnsAnEmptyWorkAreaWhenNothingIsAvailable) {
  const auto result = azookey::tsf::ResolveMonitorWorkArea({}, {500, 500});

  EXPECT_EQ(result.monitor, nullptr);
  EXPECT_EQ(result.work_area.right - result.work_area.left, 0);
}

TEST_F(CaretPositionTest, FocusLossClearsCachedCaret) {
  azookey::tsf::TextService service;
  service.set_caret_point_for_test({42, 84}, true);

  EXPECT_EQ(service.OnSetFocus(FALSE), S_OK);

  EXPECT_FALSE(service.caret_point_valid_for_test());
  const POINT point = service.caret_point_for_test();
  EXPECT_EQ(point.x, 0);
  EXPECT_EQ(point.y, 0);
}

TEST_F(CaretPositionTest, ContextPushClearsCachedCaret) {
  azookey::tsf::TextService service;
  service.set_caret_point_for_test({42, 84}, true);

  EXPECT_EQ(service.OnPushContext(nullptr), S_OK);

  EXPECT_FALSE(service.caret_point_valid_for_test());
  const POINT point = service.caret_point_for_test();
  EXPECT_EQ(point.x, 0);
  EXPECT_EQ(point.y, 0);
}

}  // namespace
