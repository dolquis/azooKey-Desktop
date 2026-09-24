#include <gtest/gtest.h>

#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azookey/core/InputState.h"
#include "azookey/core/UserActionMap.h"

namespace azookey::core {
namespace {

using Actions = std::vector<ClientAction>;

UserActionEvent Ev(UserAction action, char32_t codepoint = 0, int digit = 0) {
  return UserActionEvent{action, codepoint, 0, digit};
}

InputState Feed(InputState state, std::u32string_view text) {
  for (const char32_t cp : text) state = state.HandleEvent(Ev(UserAction::Input, cp)).next;
  return state;
}

std::vector<Candidate> KanaCandidates() {
  return {Candidate{"仮名", "かな"}, Candidate{"かな", "かな"}, Candidate{"カナ", "かな"}};
}

InputState Composing() { return Feed(InputState{}, U"kana"); }

InputState Previewing() { return Feed(InputState{}.WithLiveConversion(true), U"kana"); }

InputState Selecting() {
  const auto cached = Composing().HandleCandidatesArrived(KanaCandidates()).next;
  return cached.HandleEvent(Ev(UserAction::StartConversion)).next;
}

InputState ReplaceSuggestion() {
  return InputState{}.HandleEvent(Ev(UserAction::StartAlnumDouble)).next;
}

InputState UnicodeInput() {
  return Feed(InputState{}.HandleEvent(Ev(UserAction::StartUnicodeInput)).next, U"30");
}

constexpr UserAction kAllActions[] = {
    UserAction::Input,
    UserAction::InputAlnum,
    UserAction::Backspace,
    UserAction::Delete,
    UserAction::Forward,
    UserAction::Backward,
    UserAction::Up,
    UserAction::Down,
    UserAction::LineHead,
    UserAction::LineEnd,
    UserAction::StartConversion,
    UserAction::NextCandidate,
    UserAction::PrevCandidate,
    UserAction::SelectByDigit,
    UserAction::Commit,
    UserAction::Cancel,
    UserAction::ToggleHankaku,
    UserAction::ToggleHiraKata,
    UserAction::StartUnicodeInput,
    UserAction::StartAlnumDouble,
    UserAction::StartKanaDouble,
    UserAction::Forget,
    UserAction::ToggleDebugWindow,
};
constexpr size_t kActionCount = std::size(kAllActions);

UserActionEvent Representative(UserAction action) {
  if (action == UserAction::Input || action == UserAction::InputAlnum) return Ev(action, U'a');
  if (action == UserAction::SelectByDigit) return Ev(action, 0, 1);
  return Ev(action);
}

using K = InputStateKind;

struct Row {
  const char* name;
  InputState (*make)();
  K expected[kActionCount];
};

// Next kind for every (state, UserAction) pair, in kAllActions order.
const Row kTransitionTable[] = {
    {"Idle",
     [] { return InputState{}; },
     {K::Composing,
      K::Composing,
      K::Idle,
      K::Idle,
      K::Idle,
      K::Idle,
      K::Idle,
      K::Idle,
      K::Idle,
      K::Idle,
      K::Idle,
      K::Idle,
      K::Idle,
      K::Idle,
      K::Idle,
      K::Idle,
      K::Idle,
      K::Idle,
      K::UnicodeInput,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::Idle,
      K::Idle}},
    {"Composing",
     Composing,
     {K::Composing,    K::Composing, K::Composing, K::Composing, K::Composing, K::Composing,
      K::Composing,    K::Composing, K::Composing, K::Composing, K::Composing, K::Composing,
      K::Composing,    K::Composing, K::Idle,      K::Idle,      K::Composing, K::Composing,
      K::UnicodeInput, K::Composing, K::Composing, K::Composing, K::Composing}},
    {"Previewing",
     Previewing,
     {K::Previewing,   K::Previewing, K::Previewing, K::Previewing, K::Previewing, K::Previewing,
      K::Previewing,   K::Previewing, K::Previewing, K::Previewing, K::Previewing, K::Previewing,
      K::Previewing,   K::Previewing, K::Idle,       K::Idle,       K::Previewing, K::Previewing,
      K::UnicodeInput, K::Previewing, K::Previewing, K::Previewing, K::Previewing}},
    {"Selecting",
     Selecting,
     {K::Composing,    K::Composing, K::Composing, K::Selecting, K::Selecting, K::Selecting,
      K::Selecting,    K::Selecting, K::Selecting, K::Selecting, K::Selecting, K::Selecting,
      K::Selecting,    K::Idle,      K::Idle,      K::Composing, K::Selecting, K::Selecting,
      K::UnicodeInput, K::Selecting, K::Selecting, K::Selecting, K::Selecting}},
    {"ReplaceSuggestion",
     ReplaceSuggestion,
     {K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::Idle,
      K::Idle,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion,
      K::ReplaceSuggestion}},
    {"UnicodeInput",
     UnicodeInput,
     {K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput,
      K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput,
      K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::Idle,
      K::Idle,         K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput,
      K::UnicodeInput, K::UnicodeInput, K::UnicodeInput}},
};

TEST(InputStateTest, FixtureStatesHaveExpectedKinds) {
  EXPECT_EQ(InputState{}.kind(), K::Idle);
  EXPECT_EQ(Composing().kind(), K::Composing);
  EXPECT_EQ(Previewing().kind(), K::Previewing);
  EXPECT_EQ(Selecting().kind(), K::Selecting);
  EXPECT_EQ(ReplaceSuggestion().kind(), K::ReplaceSuggestion);
  EXPECT_EQ(UnicodeInput().kind(), K::UnicodeInput);
}

TEST(InputStateTest, EveryStateHandlesEveryUserAction) {
  for (const Row& row : kTransitionTable) {
    const InputState state = row.make();
    for (size_t i = 0; i < kActionCount; ++i) {
      const HandleResult result = state.HandleEvent(Representative(kAllActions[i]));
      EXPECT_EQ(result.next.kind(), row.expected[i])
          << row.name << " x UserAction " << static_cast<int>(kAllActions[i]);
    }
  }
}

TEST(InputStateTest, HandleEventIsPureAndIgnoresHintForM13Paths) {
  const InputState state = Composing();
  const auto event = Ev(UserAction::Input, U'k');
  const HandleResult a = state.HandleEvent(event);
  const HandleResult b = state.HandleEvent(event, EditContextHint{U'x', U'y', false});
  EXPECT_EQ(a.actions, b.actions);
  EXPECT_EQ(a.next.Reading(), b.next.Reading());
  EXPECT_EQ(state.Reading(), "かな");
}

// basic: input -> composition -> selection -> commit round trip.
TEST(InputStateTest, IdleInputStartsCompositionAndQueriesCandidates) {
  const HandleResult result = InputState{}.HandleEvent(Ev(UserAction::Input, U'k'));
  EXPECT_EQ(result.next.kind(), K::Composing);
  EXPECT_EQ(result.actions, (Actions{ReplaceMarkedText{"k"}, QueryCandidates{"k"}}));
}

TEST(InputStateTest, CandidateRoundTripCommitsSelectedCandidateAndObservesIt) {
  const InputState composing = Composing().HandleCandidatesArrived(KanaCandidates()).next;

  const HandleResult opened = composing.HandleEvent(Ev(UserAction::StartConversion));
  EXPECT_EQ(opened.next.kind(), K::Selecting);
  EXPECT_EQ(opened.actions,
            (Actions{ShowCandidateWindow{KanaCandidates(), 0}, ReplaceMarkedText{"仮名"}}));

  const HandleResult moved = opened.next.HandleEvent(Ev(UserAction::NextCandidate));
  EXPECT_EQ(moved.next.selected_index(), 1u);
  EXPECT_EQ(moved.actions, (Actions{UpdateCandidateSelection{1}, ReplaceMarkedText{"かな"}}));

  const HandleResult committed = moved.next.HandleEvent(Ev(UserAction::Commit));
  EXPECT_EQ(committed.next.kind(), K::Idle);
  EXPECT_EQ(committed.next.Reading(), "");
  EXPECT_EQ(committed.actions, (Actions{HideCandidateWindow{}, ReplaceMarkedText{"かな"},
                                        CommitMarkedText{}, ObserveCommit{"かな", "かな"}}));
}

TEST(InputStateTest, SelectingCyclesInBothDirectionsWithWrap) {
  const InputState selecting = Selecting();
  const HandleResult up = selecting.HandleEvent(Ev(UserAction::Up));
  EXPECT_EQ(up.next.selected_index(), 2u);
  EXPECT_EQ(up.actions, (Actions{UpdateCandidateSelection{2}, ReplaceMarkedText{"カナ"}}));
  EXPECT_EQ(selecting.HandleEvent(Ev(UserAction::PrevCandidate)).next.selected_index(), 2u);
  EXPECT_EQ(selecting.HandleEvent(Ev(UserAction::Down)).next.selected_index(), 1u);
  EXPECT_EQ(up.next.HandleEvent(Ev(UserAction::NextCandidate)).next.selected_index(), 0u);
}

TEST(InputStateTest, SelectByDigitCommitsThatCandidate) {
  const HandleResult result = Selecting().HandleEvent(Ev(UserAction::SelectByDigit, 0, 3));
  EXPECT_EQ(result.next.kind(), K::Idle);
  EXPECT_EQ(result.actions, (Actions{HideCandidateWindow{}, ReplaceMarkedText{"カナ"},
                                     CommitMarkedText{}, ObserveCommit{"かな", "カナ"}}));
}

TEST(InputStateTest, SelectByDigitBeyondCandidateCountKeepsSelecting) {
  const HandleResult result = Selecting().HandleEvent(Ev(UserAction::SelectByDigit, 0, 4));
  EXPECT_EQ(result.next.kind(), K::Selecting);
  EXPECT_TRUE(result.actions.empty());
}

TEST(InputStateTest, SelectingCancelReturnsToReadingAndKeepsCandidateCache) {
  const HandleResult cancelled = Selecting().HandleEvent(Ev(UserAction::Cancel));
  EXPECT_EQ(cancelled.next.kind(), K::Composing);
  EXPECT_EQ(cancelled.actions, (Actions{HideCandidateWindow{}, ReplaceMarkedText{"かな"}}));

  const HandleResult reopened = cancelled.next.HandleEvent(Ev(UserAction::StartConversion));
  EXPECT_EQ(reopened.next.kind(), K::Selecting);
}

TEST(InputStateTest, TypingWhileSelectingClosesWindowAndContinuesReading) {
  const HandleResult result = Selecting().HandleEvent(Ev(UserAction::Input, U'k'));
  EXPECT_EQ(result.next.kind(), K::Composing);
  EXPECT_TRUE(result.next.candidates().empty());
  EXPECT_EQ(result.actions,
            (Actions{HideCandidateWindow{}, ReplaceMarkedText{"かなk"}, QueryCandidates{"かなk"}}));
}

TEST(InputStateTest, TypingWhileSelectingWithLiveConversionReturnsToComposing) {
  const HandleResult result =
      Selecting().WithLiveConversion(true).HandleEvent(Ev(UserAction::Input, U'k'));
  EXPECT_EQ(result.next.kind(), K::Composing);
  EXPECT_EQ(result.next.Reading(), "かなk");
  EXPECT_EQ(result.actions,
            (Actions{HideCandidateWindow{}, ReplaceMarkedText{"かなk"}, QueryCandidates{"かなk"}}));
}

TEST(InputStateTest, ImportedCompositionPreservesPendingRomajiAndDiscardsCandidateCache) {
  RomajiKanaConverter pending;
  EXPECT_TRUE(pending.Feed('k').empty());
  const InputState imported = Selecting().WithComposition("か", pending);
  EXPECT_EQ(imported.kind(), K::Composing);
  EXPECT_EQ(imported.confirmed_kana(), "か");
  EXPECT_EQ(imported.pending_romaji().PreviewPending(), "k");
  EXPECT_EQ(imported.Reading(), "かk");
  EXPECT_TRUE(imported.candidates().empty());
  EXPECT_EQ(imported.selected_index(), 0u);
  EXPECT_FALSE(imported.awaiting_candidates());

  const HandleResult erased = imported.HandleEvent(Ev(UserAction::Backspace));
  EXPECT_EQ(erased.next.Reading(), "か");
  EXPECT_EQ(erased.actions, (Actions{ReplaceMarkedText{"か"}, QueryCandidates{"か"}}));
}

TEST(InputStateTest, ResetKeepsSettingsButDiscardsLogicalComposition) {
  const InputState reset = Composing()
                               .HandleEvent(Ev(UserAction::StartConversion))
                               .next.WithLiveConversion(true)
                               .Reset();
  EXPECT_EQ(reset.kind(), K::Idle);
  EXPECT_TRUE(reset.live_conversion());
  EXPECT_TRUE(reset.Reading().empty());
  EXPECT_TRUE(reset.candidates().empty());
  EXPECT_EQ(reset.selected_index(), 0u);
  EXPECT_FALSE(reset.awaiting_candidates());
  EXPECT_EQ(reset.WithComposition("").kind(), K::Idle);
  EXPECT_EQ(reset.WithComposition("かな").kind(), K::Composing);
}

// Cache miss: StartConversion never enters Selecting without a snapshot.
TEST(InputStateTest, CacheMissQueriesAndStaysComposing) {
  const HandleResult result = Composing().HandleEvent(Ev(UserAction::StartConversion));
  EXPECT_EQ(result.next.kind(), K::Composing);
  EXPECT_TRUE(result.next.awaiting_candidates());
  EXPECT_EQ(result.actions, (Actions{QueryCandidates{"かな"}}));
}

TEST(InputStateTest, CacheMissEnterCommitsAsIsAndDigitsPassThrough) {
  const InputState waiting = Composing().HandleEvent(Ev(UserAction::StartConversion)).next;
  EXPECT_FALSE(MapUserAction('1', 0, waiting.kind()).has_value());

  const HandleResult committed = waiting.HandleEvent(Ev(UserAction::Commit));
  EXPECT_EQ(committed.next.kind(), K::Idle);
  EXPECT_EQ(committed.actions, (Actions{ReplaceMarkedText{"かな"}, CommitMarkedText{}}));
}

TEST(InputStateTest, CandidatesArrivingAfterCacheMissOpenSelecting) {
  const InputState waiting = Composing().HandleEvent(Ev(UserAction::StartConversion)).next;
  const HandleResult arrived = waiting.HandleCandidatesArrived(KanaCandidates());
  EXPECT_EQ(arrived.next.kind(), K::Selecting);
  EXPECT_FALSE(arrived.next.awaiting_candidates());
  EXPECT_EQ(arrived.actions,
            (Actions{ShowCandidateWindow{KanaCandidates(), 0}, ReplaceMarkedText{"仮名"}}));
}

TEST(InputStateTest, EmptyResponseAfterCacheMissStopsWaiting) {
  const InputState waiting = Composing().HandleEvent(Ev(UserAction::StartConversion)).next;
  const HandleResult arrived = waiting.HandleCandidatesArrived({});
  EXPECT_EQ(arrived.next.kind(), K::Composing);
  EXPECT_FALSE(arrived.next.awaiting_candidates());
  EXPECT_TRUE(arrived.actions.empty());
}

TEST(InputStateTest, CandidatesArrivingWithoutRequestOnlyFillCache) {
  const HandleResult arrived = Composing().HandleCandidatesArrived(KanaCandidates());
  EXPECT_EQ(arrived.next.kind(), K::Composing);
  EXPECT_EQ(arrived.next.candidates().size(), 3u);
  EXPECT_TRUE(arrived.actions.empty());
}

TEST(InputStateTest, LateCandidatesDoNotReplaceTheDisplayedSnapshot) {
  const HandleResult arrived = Selecting().HandleCandidatesArrived({Candidate{"金", "かね"}});
  EXPECT_TRUE(arrived.actions.empty());
  EXPECT_TRUE(SameCandidateList(arrived.next.candidates(), KanaCandidates()));
}

TEST(InputStateTest, IdleIgnoresCandidateResponses) {
  const HandleResult arrived = InputState{}.HandleCandidatesArrived(KanaCandidates());
  EXPECT_EQ(arrived.next.kind(), K::Idle);
  EXPECT_TRUE(arrived.next.candidates().empty());
}

TEST(InputStateTest, TypingInvalidatesCandidateCache) {
  const InputState cached = Composing().HandleCandidatesArrived(KanaCandidates()).next;
  const InputState typed = cached.HandleEvent(Ev(UserAction::Input, U'k')).next;
  EXPECT_TRUE(typed.candidates().empty());
  EXPECT_EQ(typed.HandleEvent(Ev(UserAction::StartConversion)).next.kind(), K::Composing);
}

TEST(InputStateTest, StartConversionFlushesPendingRomajiAndRequeries) {
  // A trailing "n" already previews as "ん" but stays pending until flushed.
  const InputState pending = Feed(InputState{}, U"kan");
  EXPECT_EQ(pending.Reading(), "かん");
  const HandleResult result = pending.HandleEvent(Ev(UserAction::StartConversion));
  EXPECT_EQ(result.next.Reading(), "かん");
  EXPECT_EQ(result.actions, (Actions{ReplaceMarkedText{"かん"}, QueryCandidates{"かん"}}));
}

// cursor: Backspace deletes one romaji preview step or one kana.
TEST(InputStateTest, BackspaceDeletesOneUnitAndEmptiesToIdle) {
  const InputState pending = Feed(InputState{}, U"kak");
  const HandleResult romaji = pending.HandleEvent(Ev(UserAction::Backspace));
  EXPECT_EQ(romaji.next.Reading(), "か");
  EXPECT_EQ(romaji.actions, (Actions{ReplaceMarkedText{"か"}, QueryCandidates{"か"}}));

  const HandleResult emptied = romaji.next.HandleEvent(Ev(UserAction::Backspace));
  EXPECT_EQ(emptied.next.kind(), K::Idle);
  EXPECT_EQ(emptied.actions, (Actions{CancelMarkedText{}}));
}

TEST(InputStateTest, NavigationKeysDoNotEditTheReading) {
  for (const UserAction action :
       {UserAction::Delete, UserAction::Forward, UserAction::Backward, UserAction::Up,
        UserAction::Down, UserAction::LineHead, UserAction::LineEnd}) {
    const HandleResult result = Composing().HandleEvent(Ev(action));
    EXPECT_EQ(result.next.Reading(), "かな");
    EXPECT_TRUE(result.actions.empty());
  }
}

TEST(InputStateTest, BackspaceWhileSelectingEditsTheReading) {
  const HandleResult result = Selecting().HandleEvent(Ev(UserAction::Backspace));
  EXPECT_EQ(result.next.kind(), K::Composing);
  EXPECT_EQ(result.actions,
            (Actions{HideCandidateWindow{}, ReplaceMarkedText{"か"}, QueryCandidates{"か"}}));
}

TEST(InputStateTest, ComposingCancelDiscardsReading) {
  const HandleResult result = Composing().HandleEvent(Ev(UserAction::Cancel));
  EXPECT_EQ(result.next.kind(), K::Idle);
  EXPECT_EQ(result.next.Reading(), "");
  EXPECT_EQ(result.actions, (Actions{CancelMarkedText{}}));
}

TEST(InputStateTest, HyphenBecomesLongVowelAndPunctuationFlushesRomaji) {
  EXPECT_EQ(Feed(InputState{}, U"ka-").Reading(), "かー");
  EXPECT_EQ(Feed(InputState{}, U"kan、").Reading(), "かん、");
  EXPECT_EQ(Feed(InputState{}, U"ka/").Reading(), "か/");
}

TEST(InputStateTest, ExplicitPunctuationStartsCompositionAndBackspaceClearsIt) {
  for (const auto [key, surface] :
       {std::pair{vk::kOemComma, "、"}, std::pair{vk::kOemPeriod, "。"}}) {
    const auto mapped = MapUserAction(key, 0, K::Idle);
    ASSERT_TRUE(mapped);
    EXPECT_EQ(mapped->action, UserAction::Input);
    const auto inserted =
        InputState{}.HandleEvent(Ev(UserAction::Input, key == vk::kOemComma ? U'、' : U'。'));
    EXPECT_EQ(inserted.next.Reading(), surface);
    EXPECT_EQ(inserted.next.kind(), K::Composing);
    EXPECT_EQ(inserted.next.HandleEvent(Ev(UserAction::Backspace)).next.kind(), K::Idle);
    EXPECT_FALSE(MapUserAction(key, kModifierShift, K::Idle));
  }
  EXPECT_FALSE(MapUserAction(vk::kOem2, 0, K::Idle));
}

TEST(InputStateTest, InputAlnumAppendsLiterally) {
  const InputState state = InputState{}.HandleEvent(Ev(UserAction::InputAlnum, U'k')).next;
  EXPECT_EQ(state.HandleEvent(Ev(UserAction::InputAlnum, U'a')).next.Reading(), "ka");
}

// mode_toggle: switching modes keeps the reading and the candidate state.
TEST(InputStateTest, ModeTogglesKeepReadingAndEmitToggle) {
  const HandleResult hankaku = Composing().HandleEvent(Ev(UserAction::ToggleHankaku));
  EXPECT_EQ(hankaku.next.Reading(), "かな");
  EXPECT_EQ(hankaku.actions, (Actions{ToggleInputMode{InputModeToggle::Hankaku}}));

  const HandleResult hira_kata = Selecting().HandleEvent(Ev(UserAction::ToggleHiraKata));
  EXPECT_EQ(hira_kata.next.kind(), K::Selecting);
  EXPECT_EQ(hira_kata.next.selected_index(), 0u);
  EXPECT_EQ(hira_kata.actions, (Actions{ToggleInputMode{InputModeToggle::HiraKata}}));
}

TEST(InputStateTest, ForgetAndDebugWindowKeepState) {
  EXPECT_EQ(InputState{}.HandleEvent(Ev(UserAction::Forget)).actions,
            (Actions{ForgetLastCommit{}}));
  const HandleResult debug = Composing().HandleEvent(Ev(UserAction::ToggleDebugWindow));
  EXPECT_EQ(debug.next.Reading(), "かな");
  EXPECT_EQ(debug.actions, (Actions{ToggleDebugWindow{}}));
}

TEST(InputStateTest, LiveConversionTurnsComposingIntoPreviewing) {
  const InputState first =
      InputState{}.WithLiveConversion(true).HandleEvent(Ev(UserAction::Input, U'k')).next;
  EXPECT_EQ(first.kind(), K::Composing);
  EXPECT_EQ(first.HandleEvent(Ev(UserAction::Input, U'a')).next.kind(), K::Previewing);
}

TEST(InputStateTest, PreviewingStartConversionUsesCacheLikeComposing) {
  const InputState cached = Previewing().HandleCandidatesArrived(KanaCandidates()).next;
  EXPECT_EQ(cached.HandleEvent(Ev(UserAction::StartConversion)).next.kind(), K::Selecting);
  const HandleResult miss = Previewing().HandleEvent(Ev(UserAction::StartConversion));
  EXPECT_EQ(miss.next.kind(), K::Previewing);
  EXPECT_TRUE(miss.next.awaiting_candidates());
}

TEST(InputStateTest, ReplaceSuggestionPromptModesAndExit) {
  EXPECT_EQ(InputState{}.HandleEvent(Ev(UserAction::StartAlnumDouble)).actions,
            (Actions{ShowReplaceSuggestionPrompt{ReplaceSuggestionMode::MagicConversion}}));
  const HandleResult kana = InputState{}.HandleEvent(Ev(UserAction::StartKanaDouble));
  EXPECT_EQ(kana.next.replace_suggestion_mode(), ReplaceSuggestionMode::ReplaceSuggestion);
  EXPECT_EQ(kana.actions,
            (Actions{ShowReplaceSuggestionPrompt{ReplaceSuggestionMode::ReplaceSuggestion}}));
  EXPECT_EQ(ReplaceSuggestion().HandleEvent(Ev(UserAction::Commit)).actions,
            (Actions{SubmitReplaceSuggestion{}}));
  EXPECT_EQ(ReplaceSuggestion().HandleEvent(Ev(UserAction::Cancel)).actions,
            (Actions{HideReplaceSuggestionPrompt{}}));
}

TEST(InputStateTest, UnicodeInputShowsPrefixedHexAndCommitsScalar) {
  const HandleResult started = InputState{}.HandleEvent(Ev(UserAction::StartUnicodeInput));
  EXPECT_EQ(started.actions, (Actions{ReplaceMarkedText{"U+"}}));

  const HandleResult typed = UnicodeInput().HandleEvent(Ev(UserAction::Input, U'a'));
  EXPECT_EQ(typed.next.unicode_hex(), "30A");
  EXPECT_EQ(typed.actions, (Actions{ReplaceMarkedText{"U+30A"}}));

  const InputState full = Feed(UnicodeInput(), U"42");
  const HandleResult committed = full.HandleEvent(Ev(UserAction::Commit));
  EXPECT_EQ(committed.next.kind(), K::Idle);
  EXPECT_EQ(committed.actions, (Actions{ReplaceMarkedText{"あ"}, CommitMarkedText{}}));
}

TEST(InputStateTest, UnicodeInputCommitsSupplementaryScalar) {
  const InputState state =
      Feed(InputState{}.HandleEvent(Ev(UserAction::StartUnicodeInput)).next, U"1F600");
  EXPECT_EQ(state.HandleEvent(Ev(UserAction::Commit)).actions,
            (Actions{ReplaceMarkedText{"😀"}, CommitMarkedText{}}));
}

TEST(InputStateTest, UnicodeInputRejectsSurrogatesAndOutOfRange) {
  for (const std::u32string_view hex :
       {std::u32string_view{U"D800"}, std::u32string_view{U"110000"}}) {
    const InputState state =
        Feed(InputState{}.HandleEvent(Ev(UserAction::StartUnicodeInput)).next, hex);
    const HandleResult result = state.HandleEvent(Ev(UserAction::Commit));
    EXPECT_EQ(result.next.kind(), K::UnicodeInput);
    EXPECT_EQ(result.actions, (Actions{PlayBeep{}}));
  }
}

TEST(InputStateTest, UnicodeInputLimitsHexDigits) {
  const InputState full =
      Feed(InputState{}.HandleEvent(Ev(UserAction::StartUnicodeInput)).next, U"00000041");
  const HandleResult result = full.HandleEvent(Ev(UserAction::Input, U'1'));
  EXPECT_EQ(result.next.unicode_hex(), "00000041");
  EXPECT_EQ(result.actions, (Actions{PlayBeep{}}));
}

TEST(InputStateTest, UnicodeInputNonHexKeyConfirmsAndContinuesAsInput) {
  const InputState state = Feed(UnicodeInput(), U"42");
  const HandleResult result = state.HandleEvent(Ev(UserAction::Input, U'k'));
  EXPECT_EQ(result.next.kind(), K::Composing);
  EXPECT_EQ(result.next.Reading(), "k");
  EXPECT_EQ(result.actions, (Actions{ReplaceMarkedText{"あ"}, CommitMarkedText{},
                                     ReplaceMarkedText{"k"}, QueryCandidates{"k"}}));
}

TEST(InputStateTest, UnicodeInputBackspaceAndEmptyExit) {
  const HandleResult popped = UnicodeInput().HandleEvent(Ev(UserAction::Backspace));
  EXPECT_EQ(popped.actions, (Actions{ReplaceMarkedText{"U+3"}}));
  const InputState empty = InputState{}.HandleEvent(Ev(UserAction::StartUnicodeInput)).next;
  const HandleResult exited = empty.HandleEvent(Ev(UserAction::Backspace));
  EXPECT_EQ(exited.next.kind(), K::Idle);
  EXPECT_EQ(exited.actions, (Actions{CancelMarkedText{}}));
  EXPECT_EQ(empty.HandleEvent(Ev(UserAction::Commit)).actions, (Actions{CancelMarkedText{}}));
}

TEST(InputStateTest, StartUnicodeInputFromCompositionCommitsReadingFirst) {
  const HandleResult result = Composing().HandleEvent(Ev(UserAction::StartUnicodeInput));
  EXPECT_EQ(result.next.kind(), K::UnicodeInput);
  EXPECT_EQ(result.actions,
            (Actions{ReplaceMarkedText{"かな"}, CommitMarkedText{}, ReplaceMarkedText{"U+"}}));
}

}  // namespace
}  // namespace azookey::core
