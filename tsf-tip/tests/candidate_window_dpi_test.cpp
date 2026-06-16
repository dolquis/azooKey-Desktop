#include <gtest/gtest.h>

#include "azookey/tsf/CandidateWindow.h"

namespace azookey::tsf {
namespace {

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

TEST(CandidateWindowDpiTest, ZeroDpiFallsBackToDefaultDpi) {
  ExpectMetrics(CandidateWindow::ComputeLayoutMetricsForTest(0), 24, 8, 400, 20, 60, 4);
}

}  // namespace
}  // namespace azookey::tsf
