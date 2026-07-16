#include <gtest/gtest.h>

#include <cstdint>

#include "azookey/tsf/TextService.h"

namespace {

int g_gui_thread_info_calls = 0;
int g_client_to_screen_calls = 0;
int g_cursor_pos_calls = 0;
RECT g_gui_caret_rect{};
bool g_gui_thread_info_succeeds = false;
bool g_client_to_screen_succeeds = false;
bool g_cursor_pos_succeeds = false;
POINT g_screen_offset{};
POINT g_cursor_point{};

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

BOOL WINAPI FakeGetCursorPos(LPPOINT point) {
  ++g_cursor_pos_calls;
  if (!g_cursor_pos_succeeds) return FALSE;
  *point = g_cursor_point;
  return TRUE;
}

class CaretPositionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_gui_thread_info_calls = 0;
    g_client_to_screen_calls = 0;
    g_cursor_pos_calls = 0;
    g_gui_caret_rect = {};
    g_gui_thread_info_succeeds = false;
    g_client_to_screen_succeeds = false;
    g_cursor_pos_succeeds = false;
    g_screen_offset = {};
    g_cursor_point = {};
    azookey::tsf::testing::SetCaretWin32ApiForTest(&FakeGetGuiThreadInfo, &FakeClientToScreen,
                                                   &FakeGetCursorPos);
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
  EXPECT_EQ(g_cursor_pos_calls, 0);
}

TEST_F(CaretPositionTest, EmptyTextExtentFallsBackToGuiThreadCaretInScreenCoordinates) {
  const RECT empty_text_extent{10, 20, 10, 44};
  g_gui_thread_info_succeeds = true;
  g_client_to_screen_succeeds = true;
  g_gui_caret_rect = {4, 5, 6, 25};
  g_screen_offset = {100, 200};

  const auto anchor = azookey::tsf::testing::ResolveCaretAnchorForTest(&empty_text_extent);

  EXPECT_TRUE(anchor.valid);
  EXPECT_EQ(anchor.point.x, 104);
  EXPECT_EQ(anchor.point.y, 225);
  EXPECT_EQ(g_gui_thread_info_calls, 1);
  EXPECT_EQ(g_client_to_screen_calls, 1);
  EXPECT_EQ(g_cursor_pos_calls, 0);
}

TEST_F(CaretPositionTest, ZeroGuiCaretFallsBackToCursorPosition) {
  g_gui_thread_info_succeeds = true;
  g_cursor_pos_succeeds = true;
  g_cursor_point = {321, 654};

  const auto anchor = azookey::tsf::testing::ResolveCaretAnchorForTest(nullptr);

  EXPECT_TRUE(anchor.valid);
  EXPECT_EQ(anchor.point.x, 321);
  EXPECT_EQ(anchor.point.y, 654);
  EXPECT_EQ(g_gui_thread_info_calls, 1);
  EXPECT_EQ(g_client_to_screen_calls, 0);
  EXPECT_EQ(g_cursor_pos_calls, 1);
}

TEST_F(CaretPositionTest, AllFailuresReturnInvalidOrigin) {
  const auto anchor = azookey::tsf::testing::ResolveCaretAnchorForTest(nullptr);

  EXPECT_FALSE(anchor.valid);
  EXPECT_EQ(anchor.point.x, 0);
  EXPECT_EQ(anchor.point.y, 0);
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

}  // namespace
