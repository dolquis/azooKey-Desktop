#include <gtest/gtest.h>

#include <variant>

#include "azookey/core/InputState.h"

namespace azookey::core {
namespace {

TEST(M59InputStateTest, BackspaceEditsReadingAndNeverCountsDerivedPunctuation) {
  auto state = InputState{}.WithLiveConversion(true).WithComposition("きょうは");
  state = state.HandleCandidatesArrived({Candidate{"今日は。", "きょうは"}}).next;
  const auto result = state.HandleEvent({UserAction::Backspace});
  EXPECT_EQ(result.next.Reading(), "きょう");
  EXPECT_EQ(result.next.kind(), InputStateKind::Composing);
  ASSERT_EQ(result.actions.size(), 2u);
  ASSERT_TRUE(std::holds_alternative<ReplaceMarkedText>(result.actions.front()));
  EXPECT_EQ(std::get<ReplaceMarkedText>(result.actions.front()).text, "きょう");
  EXPECT_TRUE(std::holds_alternative<QueryCandidates>(result.actions.back()));
}

}  // namespace
}  // namespace azookey::core
