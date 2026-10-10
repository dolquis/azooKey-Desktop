#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "../src/StderrLineQueue.h"

namespace {

using azookey::host::detail::StderrLineQueue;

TEST(StderrLineQueueTest, AcceptsCompleteLineAtDefaultLimit) {
  StderrLineQueue queue;
  constexpr std::size_t kDefaultLimit = 64 * 1024;
  const std::string line = std::string(kDefaultLimit - 1, 'x') + '\n';

  queue.Append(line);

  EXPECT_EQ(queue.Take(), line);
  EXPECT_TRUE(queue.Take().empty());
}

TEST(StderrLineQueueTest, DropsWholeLineWhenRemainingCapacityIsTooSmall) {
  StderrLineQueue queue(10);
  queue.Append("first\n");
  queue.Append("ABCDE\n");

  EXPECT_EQ(queue.Take(), "first\n");
  EXPECT_TRUE(queue.Take().empty());

  queue.Append("ok\n");
  EXPECT_EQ(queue.Take(), "ok\n");
}

TEST(StderrLineQueueTest, KeepsFragmentsUntilNewline) {
  StderrLineQueue queue(16);
  queue.Append("alpha");
  EXPECT_TRUE(queue.Take().empty());

  queue.Append("beta");
  EXPECT_TRUE(queue.Take().empty());

  queue.Append("\n");
  EXPECT_EQ(queue.Take(), "alphabeta\n");
  EXPECT_TRUE(queue.Take().empty());
}

TEST(StderrLineQueueTest, TakesCompletedLinesWithoutTakingTrailingFragment) {
  StderrLineQueue queue(16);
  queue.Append("one\npart");

  EXPECT_EQ(queue.Take(), "one\n");
  EXPECT_TRUE(queue.Take().empty());

  queue.Append("ial\n");
  EXPECT_EQ(queue.Take(), "partial\n");
}

TEST(StderrLineQueueTest, DropsOversizedFragmentedLineThroughNewlineThenRecovers) {
  StderrLineQueue queue(8);
  queue.Append("12345678");
  EXPECT_TRUE(queue.Take().empty());

  queue.Append("oversized-tail");
  EXPECT_TRUE(queue.Take().empty());

  queue.Append("\nok\n");
  EXPECT_EQ(queue.Take(), "ok\n");
  EXPECT_TRUE(queue.Take().empty());
}

TEST(StderrLineQueueTest, AcceptsNewLinesAfterFullQueueIsDrained) {
  StderrLineQueue queue(8);
  queue.Append("1234567\n");
  queue.Append("drop\n");

  EXPECT_EQ(queue.Take(), "1234567\n");
  EXPECT_TRUE(queue.Take().empty());

  queue.Append("next\n");
  EXPECT_EQ(queue.Take(), "next\n");
}

TEST(StderrLineQueueTest, KeepsOnlyFittingWholeLinesFromSingleAppend) {
  StderrLineQueue queue(10);
  queue.Append("one\nseven77\nok\n");

  EXPECT_EQ(queue.Take(), "one\nok\n");
  EXPECT_TRUE(queue.Take().empty());
}

}  // namespace
