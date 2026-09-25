#include <gtest/gtest.h>

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

}  // namespace
}  // namespace azookey::tsf
