#include <gtest/gtest.h>

#include <cstdint>

#include "azookey/tsf/TextService.h"

TEST(TsfTipStalenessTest, AcceptsOnlyTheLatestOfTenRapidResponses) {
  constexpr uint64_t kLatestRequestId = 10;
  size_t accepted = 0;

  for (uint64_t response_id = 1; response_id <= kLatestRequestId; ++response_id) {
    if (azookey::tsf::testing::IsFreshQueryResultForTest(false, kLatestRequestId, response_id)) {
      ++accepted;
    }
  }

  EXPECT_EQ(accepted, 1u);
  EXPECT_TRUE(
      azookey::tsf::testing::IsFreshQueryResultForTest(false, kLatestRequestId, kLatestRequestId));
}

TEST(TsfTipStalenessTest, RejectsResponseWhenANewerRequestIsQueued) {
  EXPECT_FALSE(azookey::tsf::testing::IsFreshQueryResultForTest(true, 10, 10));
}

TEST(TsfTipStalenessTest, RejectsResponseInvalidatedByCommit) {
  EXPECT_FALSE(azookey::tsf::testing::IsFreshQueryResultForTest(false, 11, 10));
}
