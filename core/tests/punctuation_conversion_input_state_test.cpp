#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "azookey/core/InputState.h"

// DEV-1638: punctuationConversion = "commit" (docs/tsf-deep-integration-spec.md
// §3.5.1). With the setting off a 、 or 。 key only joins the reading.
namespace azookey::core {
namespace {

using Actions = std::vector<ClientAction>;
using K = InputStateKind;

UserActionEvent PunctuationEvent(UserAction action, char32_t codepoint = 0, int digit = 0) {
  return UserActionEvent{action, codepoint, 0, digit};
}

InputState FeedPunctuationState(InputState state, std::u32string_view text) {
  for (const char32_t cp : text)
    state = state.HandleEvent(PunctuationEvent(UserAction::Input, cp)).next;
  return state;
}

std::vector<Candidate> TenkiCandidates() {
  return {Candidate{"天気", "てんき"}, Candidate{"転機", "てんき"}};
}

// Composing "てんき" with the per-key first candidate cached.
InputState CachedComposing(bool conversion, bool live = false) {
  const auto typed = FeedPunctuationState(
      InputState{}.WithLiveConversion(live).WithPunctuationConversion(conversion), U"tenki");
  return typed.HandleCandidatesArrived(TenkiCandidates()).next;
}

InputState SelectingSecond(bool conversion) {
  const auto waiting =
      CachedComposing(conversion).HandleEvent(PunctuationEvent(UserAction::StartConversion));
  const auto opened = waiting.next.HandleCandidatesArrived(TenkiCandidates()).next;
  return opened.HandleEvent(PunctuationEvent(UserAction::NextCandidate)).next;
}

const UserActionEvent kRepresentativeEvents[] = {
    PunctuationEvent(UserAction::Input, U'a'),
    PunctuationEvent(UserAction::Input, U'、'),
    PunctuationEvent(UserAction::Input, U'。'),
    PunctuationEvent(UserAction::Input, U'/'),
    PunctuationEvent(UserAction::InputAlnum, U'。'),
    PunctuationEvent(UserAction::Backspace),
    PunctuationEvent(UserAction::StartConversion),
    PunctuationEvent(UserAction::NextCandidate),
    PunctuationEvent(UserAction::SelectByDigit, 0, 1),
    PunctuationEvent(UserAction::Commit),
    PunctuationEvent(UserAction::Cancel),
    PunctuationEvent(UserAction::StartUnicodeInput),
};

void ExpectSameResult(const InputState& off, const InputState& on, const UserActionEvent& event,
                      const char* name) {
  const HandleResult a = off.HandleEvent(event);
  const HandleResult b = on.HandleEvent(event);
  EXPECT_EQ(a.actions, b.actions) << name << " codepoint "
                                  << static_cast<uint32_t>(event.codepoint);
  EXPECT_EQ(a.next.kind(), b.next.kind()) << name;
  EXPECT_EQ(a.next.Reading(), b.next.Reading()) << name;
}

TEST(PunctuationConversionInputStateTest, DefaultsOffAndKeepsTheSettingAcrossReset) {
  EXPECT_FALSE(InputState{}.punctuation_conversion());
  const auto on = InputState{}.WithPunctuationConversion(true);
  EXPECT_TRUE(on.punctuation_conversion());
  EXPECT_TRUE(FeedPunctuationState(on, U"ka").Reset().punctuation_conversion());
}

TEST(PunctuationConversionInputStateTest, OffLeavesEveryStateAndKeyUnchanged) {
  const struct {
    const char* name;
    InputState off;
    InputState on;
  } states[] = {
      {"Idle", InputState{}, InputState{}.WithPunctuationConversion(true)},
      {"Composing", CachedComposing(false), CachedComposing(false).WithPunctuationConversion(true)},
      {"Selecting", SelectingSecond(false), SelectingSecond(false).WithPunctuationConversion(true)},
  };
  // The setting must not change any event except a 、 or 。 that converts.
  for (const auto& state : states) {
    for (const auto& event : kRepresentativeEvents) {
      const bool converts = state.on.PunctuationConversionReading(event).has_value();
      if (!converts) ExpectSameResult(state.off, state.on, event, state.name);
    }
  }
  // Off: the marks join the reading as before (spec §3.5, DEV-847 G2).
  for (const char32_t mark : {U'、', U'。'}) {
    const auto joined =
        CachedComposing(false).HandleEvent(PunctuationEvent(UserAction::Input, mark));
    EXPECT_EQ(joined.next.kind(), K::Composing);
    EXPECT_EQ(joined.next.Reading(), mark == U'、' ? "てんき、" : "てんき。");
    const auto reopened =
        SelectingSecond(false).HandleEvent(PunctuationEvent(UserAction::Input, mark));
    EXPECT_EQ(reopened.next.kind(), K::Composing);
  }
}

TEST(PunctuationConversionInputStateTest, ComposingCommitsFirstCandidateWithTheMark) {
  const auto result = CachedComposing(true).HandleEvent(PunctuationEvent(UserAction::Input, U'。'));
  EXPECT_EQ(result.next.kind(), K::Idle);
  EXPECT_EQ(result.next.Reading(), "");
  EXPECT_EQ(result.actions, (Actions{HideCandidateWindow{}, ReplaceMarkedText{"天気。"},
                                     CommitMarkedText{}, ObserveCommit{"てんき", "天気"}}));
}

TEST(PunctuationConversionInputStateTest, PreviewingCommitsTheInjectedFirstCandidate) {
  const auto result =
      CachedComposing(true, true).HandleEvent(PunctuationEvent(UserAction::Input, U'、'));
  EXPECT_EQ(result.next.kind(), K::Idle);
  EXPECT_EQ(result.actions, (Actions{HideCandidateWindow{}, ReplaceMarkedText{"天気、"},
                                     CommitMarkedText{}, ObserveCommit{"てんき", "天気"}}));
}

TEST(PunctuationConversionInputStateTest, SelectingCommitsTheHighlightWithTheMark) {
  const auto result = SelectingSecond(true).HandleEvent(PunctuationEvent(UserAction::Input, U'、'));
  EXPECT_EQ(result.next.kind(), K::Idle);
  EXPECT_EQ(result.actions, (Actions{HideCandidateWindow{}, ReplaceMarkedText{"転機、"},
                                     CommitMarkedText{}, ObserveCommit{"てんき", "転機"}}));
}

TEST(PunctuationConversionInputStateTest, WithoutACandidateForTheReadingTheMarkJoins) {
  // Nothing cached yet.
  const auto uncached =
      FeedPunctuationState(InputState{}.WithPunctuationConversion(true), U"tenki");
  ASSERT_TRUE(uncached.PunctuationConversionReading(PunctuationEvent(UserAction::Input, U'。')));
  EXPECT_EQ(uncached.HandleEvent(PunctuationEvent(UserAction::Input, U'。')).next.Reading(),
            "てんき。");
  // A cache of another reading (pending romaji flushes into a new one).
  const auto pending = FeedPunctuationState(InputState{}.WithPunctuationConversion(true), U"kan")
                           .HandleCandidatesArrived({Candidate{"かn", "かn"}})
                           .next;
  EXPECT_EQ(*pending.PunctuationConversionReading(PunctuationEvent(UserAction::Input, U'、')),
            "かん");
  const auto joined = pending.HandleEvent(PunctuationEvent(UserAction::Input, U'、'));
  EXPECT_EQ(joined.next.kind(), K::Composing);
  EXPECT_EQ(joined.next.Reading(), "かん、");
  // A cache of the flushed reading commits it.
  const auto flushed = FeedPunctuationState(InputState{}.WithPunctuationConversion(true), U"kan")
                           .HandleCandidatesArrived({Candidate{"缶", "かん"}})
                           .next;
  EXPECT_EQ(flushed.HandleEvent(PunctuationEvent(UserAction::Input, U'、')).actions,
            (Actions{HideCandidateWindow{}, ReplaceMarkedText{"缶、"}, CommitMarkedText{},
                     ObserveCommit{"かん", "缶"}}));
}

TEST(PunctuationConversionInputStateTest, PunctuationOnlyDigitsAndOtherKeysDoNotConvert) {
  const auto on = InputState{}.WithPunctuationConversion(true);
  // Idle: the mark starts a standalone composition with half-width candidates.
  EXPECT_FALSE(on.PunctuationConversionReading(PunctuationEvent(UserAction::Input, U'、')));
  const auto standalone = FeedPunctuationState(on, U"、");
  EXPECT_FALSE(standalone.PunctuationConversionReading(PunctuationEvent(UserAction::Input, U'。')));
  EXPECT_EQ(standalone.HandleEvent(PunctuationEvent(UserAction::Input, U'。')).next.Reading(),
            "、。");
  // A mark right after a digit is a decimal point or a separator.
  for (const auto digits : {std::u32string_view{U"3"}, std::u32string_view{U"３"}}) {
    const auto number = FeedPunctuationState(on, digits)
                            .HandleCandidatesArrived({Candidate{"3", "3"}, Candidate{"３", "３"}})
                            .next;
    EXPECT_FALSE(number.PunctuationConversionReading(PunctuationEvent(UserAction::Input, U'。')));
  }
  // Only 、 and 。 typed as Input convert.
  const auto composing = CachedComposing(true);
  EXPECT_FALSE(composing.PunctuationConversionReading(PunctuationEvent(UserAction::Input, U'/')));
  EXPECT_FALSE(composing.PunctuationConversionReading(PunctuationEvent(UserAction::Input, U'?')));
  EXPECT_FALSE(
      composing.PunctuationConversionReading(PunctuationEvent(UserAction::InputAlnum, U'。')));
  EXPECT_FALSE(composing.PunctuationConversionReading(PunctuationEvent(UserAction::Commit)));
}

TEST(PunctuationConversionInputStateTest, SelectingStandalonePunctuationDoesNotConvert) {
  const auto waiting = FeedPunctuationState(InputState{}.WithPunctuationConversion(true), U"、")
                           .HandleEvent(PunctuationEvent(UserAction::StartConversion))
                           .next;
  const auto selecting =
      waiting.HandleCandidatesArrived({Candidate{"、", "、"}, Candidate{",", "、"}}).next;
  ASSERT_EQ(selecting.kind(), K::Selecting);
  const auto typed = selecting.HandleEvent(PunctuationEvent(UserAction::Input, U'。'));
  EXPECT_EQ(typed.next.kind(), K::Composing);
  EXPECT_EQ(typed.next.Reading(), "、。");
}

}  // namespace
}  // namespace azookey::core
