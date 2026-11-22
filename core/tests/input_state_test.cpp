#include <gtest/gtest.h>

#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azookey/core/CandidatePage.h"
#include "azookey/core/CustomRomajiLoader.h"
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
  const auto waiting = Composing().HandleEvent(Ev(UserAction::StartConversion)).next;
  return waiting.HandleCandidatesArrived(KanaCandidates()).next;
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
    UserAction::ToggleAlnum,
    UserAction::CommitHiragana,
    UserAction::CommitKatakana,
    UserAction::CommitHalfwidthKatakana,
    UserAction::CommitFullwidthRoman,
    UserAction::CommitHalfwidthRoman,
    UserAction::UndoCommit,
    UserAction::ShrinkSegment,
    UserAction::ExtendSegment,
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

// Next kind for each representative state fixture, in kAllActions order.
// Selecting uses the whole reading. F6-F10 after resizing are covered by
// SegmentEditingTest.CharacterFormKeysWhileResizedAskForTheRemainingReading.
const Row kTransitionTable[] = {
    {"Idle",
     [] { return InputState{}; },
     {K::Composing, K::Composing, K::Idle,         K::Idle, K::Idle, K::Idle, K::Idle, K::Idle,
      K::Idle,      K::Idle,      K::Idle,         K::Idle, K::Idle, K::Idle, K::Idle, K::Idle,
      K::Idle,      K::Idle,      K::UnicodeInput, K::Idle, K::Idle, K::Idle, K::Idle, K::Idle,
      K::Idle,      K::Idle,      K::Idle,         K::Idle, K::Idle, K::Idle, K::Idle, K::Idle}},
    {"Composing",
     Composing,
     {K::Composing,    K::Composing, K::Composing, K::Composing, K::Composing, K::Composing,
      K::Composing,    K::Composing, K::Composing, K::Composing, K::Composing, K::Composing,
      K::Composing,    K::Composing, K::Idle,      K::Idle,      K::Composing, K::Composing,
      K::UnicodeInput, K::Composing, K::Composing, K::Composing, K::Composing, K::Composing,
      K::Composing,    K::Composing, K::Composing, K::Composing, K::Composing, K::Composing,
      K::Composing,    K::Composing}},
    {"Previewing",
     Previewing,
     {K::Previewing,   K::Previewing, K::Previewing, K::Previewing, K::Previewing, K::Previewing,
      K::Previewing,   K::Previewing, K::Previewing, K::Previewing, K::Previewing, K::Previewing,
      K::Previewing,   K::Previewing, K::Idle,       K::Idle,       K::Previewing, K::Previewing,
      K::UnicodeInput, K::Previewing, K::Previewing, K::Previewing, K::Previewing, K::Previewing,
      K::Previewing,   K::Previewing, K::Previewing, K::Previewing, K::Previewing, K::Previewing,
      K::Previewing,   K::Previewing}},
    {"Selecting",
     Selecting,
     {K::Composing,    K::Composing, K::Composing, K::Selecting, K::Selecting, K::Selecting,
      K::Selecting,    K::Selecting, K::Selecting, K::Selecting, K::Selecting, K::Selecting,
      K::Selecting,    K::Idle,      K::Idle,      K::Idle,      K::Selecting, K::Selecting,
      K::UnicodeInput, K::Selecting, K::Selecting, K::Selecting, K::Selecting, K::Selecting,
      K::Selecting,    K::Selecting, K::Selecting, K::Selecting, K::Selecting, K::Selecting,
      K::Selecting,    K::Selecting}},
    {"UnicodeInput",
     UnicodeInput,
     {K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput,
      K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput,
      K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::Idle,
      K::Idle,         K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput,
      K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput,
      K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput, K::UnicodeInput,
      K::UnicodeInput, K::UnicodeInput}},
};

TEST(InputStateTest, FixtureStatesHaveExpectedKinds) {
  EXPECT_EQ(InputState{}.kind(), K::Idle);
  EXPECT_EQ(Composing().kind(), K::Composing);
  EXPECT_EQ(Previewing().kind(), K::Previewing);
  EXPECT_EQ(Selecting().kind(), K::Selecting);
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
  const HandleResult queried = Composing().HandleEvent(Ev(UserAction::StartConversion));
  EXPECT_EQ(queried.actions, (Actions{QueryCandidates{"かな", true}}));

  const HandleResult opened = queried.next.HandleCandidatesArrived(KanaCandidates());
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

// M68 (legacy-parity-spec §1.4「候補のページ」): nine candidates to a page.
std::vector<Candidate> NumberedCandidates(size_t count) {
  std::vector<Candidate> candidates;
  for (size_t i = 0; i < count; ++i) candidates.push_back(Candidate{std::to_string(i), "かな"});
  return candidates;
}

InputState SelectingAt(size_t count, size_t selected) {
  const auto waiting = Composing().HandleEvent(Ev(UserAction::StartConversion)).next;
  InputState state = waiting.HandleCandidatesArrived(NumberedCandidates(count)).next;
  for (size_t i = 0; i < selected; ++i) state = state.HandleEvent(Ev(UserAction::Down)).next;
  return state;
}

UserActionEvent PageEv(UserAction action, std::vector<uint32_t> page_starts = {}) {
  UserActionEvent event = Ev(action);
  event.by_page = true;
  event.page_starts = std::move(page_starts);
  return event;
}

UserActionEvent DigitEv(int digit, std::vector<uint32_t> page_starts = {}) {
  UserActionEvent event = Ev(UserAction::SelectByDigit, 0, digit);
  event.page_starts = std::move(page_starts);
  return event;
}

TEST(CandidatePageTest, DefaultSplitIsNinePerPageFromTheFirstCandidate) {
  EXPECT_EQ(CandidatePageStarts(0), (std::vector<uint32_t>{}));
  EXPECT_EQ(CandidatePageStarts(9), (std::vector<uint32_t>{0}));
  EXPECT_EQ(CandidatePageStarts(20), (std::vector<uint32_t>{0, 9, 18}));
  const CandidatePageRange last = CandidatePageAt(19, 20);
  EXPECT_EQ(last.page, 2u);
  EXPECT_EQ(last.begin, 18u);
  EXPECT_EQ(last.end, 20u);
  EXPECT_EQ(CandidatePageAt(8, 20).end, 9u);
  EXPECT_EQ(CandidatePageAt(9, 20).begin, 9u);
  // An index past the list belongs to the last page.
  EXPECT_EQ(CandidatePageAt(40, 20).page, 2u);
}

TEST(CandidatePageTest, AppSplitAppliesOnlyWhenItStartsAtZeroAndStrictlyIncreasesBelowTheCount) {
  EXPECT_TRUE(IsValidCandidatePageStarts({0, 4, 10}, 12));
  EXPECT_EQ(CandidatePageStarts(12, {0, 4, 10}), (std::vector<uint32_t>{0, 4, 10}));
  for (const auto& invalid :
       std::vector<std::vector<uint32_t>>{{}, {1, 4}, {0, 4, 4}, {0, 6, 4}, {0, 12}, {0, 4, 13}}) {
    EXPECT_FALSE(IsValidCandidatePageStarts(invalid, 12));
    // An unusable split falls back to nine per page.
    EXPECT_EQ(CandidatePageStarts(12, invalid), (std::vector<uint32_t>{0, 9}));
  }
}

TEST(InputStateTest, PageDownAndPageUpMoveToTheSamePositionOnTheAdjacentPage) {
  // 20 candidates: [0, 9), [9, 18), [18, 20).
  const InputState third = SelectingAt(20, 2);
  const HandleResult down = third.HandleEvent(PageEv(UserAction::Down));
  EXPECT_EQ(down.next.kind(), K::Selecting);
  EXPECT_EQ(down.next.selected_index(), 11u);
  EXPECT_EQ(down.actions, (Actions{UpdateCandidateSelection{11}, ReplaceMarkedText{"11"}}));
  // The short last page has no third candidate: its last one instead.
  const HandleResult last = down.next.HandleEvent(PageEv(UserAction::Down));
  EXPECT_EQ(last.next.selected_index(), 19u);
  // The last page wraps to the first, and the first back to the last.
  EXPECT_EQ(last.next.HandleEvent(PageEv(UserAction::Down)).next.selected_index(), 1u);
  EXPECT_EQ(SelectingAt(20, 0).HandleEvent(PageEv(UserAction::Up)).next.selected_index(), 18u);
  EXPECT_EQ(down.next.HandleEvent(PageEv(UserAction::Up)).next.selected_index(), 2u);
  // Up / Down without the page flag still move one candidate across the page edge.
  EXPECT_EQ(SelectingAt(20, 8).HandleEvent(Ev(UserAction::Down)).next.selected_index(), 9u);
}

TEST(InputStateTest, PageKeysDoNothingWithOnePage) {
  for (const size_t count : {size_t{3}, size_t{9}}) {
    SCOPED_TRACE(count);
    const InputState selecting = SelectingAt(count, 1);
    for (const UserAction action : {UserAction::Up, UserAction::Down}) {
      const HandleResult result = selecting.HandleEvent(PageEv(action));
      EXPECT_EQ(result.next.kind(), K::Selecting);
      EXPECT_EQ(result.next.selected_index(), 1u);
      EXPECT_TRUE(result.actions.empty());
    }
  }
}

TEST(InputStateTest, SelectByDigitCommitsTheNthCandidateOfTheCurrentPage) {
  // The selection on the second page: 3 picks the twelfth candidate.
  const HandleResult second = SelectingAt(20, 10).HandleEvent(DigitEv(3));
  EXPECT_EQ(second.next.kind(), K::Idle);
  EXPECT_EQ(second.actions, (Actions{HideCandidateWindow{}, ReplaceMarkedText{"11"},
                                     CommitMarkedText{}, ObserveCommit{"かな", "11"}}));
  // The last page holds two: 3 does nothing and keeps the window.
  const HandleResult missing = SelectingAt(20, 18).HandleEvent(DigitEv(3));
  EXPECT_EQ(missing.next.kind(), K::Selecting);
  EXPECT_EQ(missing.next.selected_index(), 18u);
  EXPECT_TRUE(missing.actions.empty());
  EXPECT_EQ(SelectingAt(20, 18).HandleEvent(DigitEv(2)).actions.at(1),
            ClientAction{ReplaceMarkedText{"19"}});
}

TEST(InputStateTest, PageKeysAndDigitsFollowTheSplitTheFrontendAttaches) {
  // An app split of 12 candidates: [0, 4), [4, 10), [10, 12).
  const std::vector<uint32_t> split{0, 4, 10};
  EXPECT_EQ(SelectingAt(12, 2).HandleEvent(PageEv(UserAction::Down, split)).next.selected_index(),
            6u);
  EXPECT_EQ(SelectingAt(12, 2).HandleEvent(PageEv(UserAction::Up, split)).next.selected_index(),
            11u);
  EXPECT_EQ(SelectingAt(12, 5).HandleEvent(DigitEv(6, split)).actions.at(1),
            ClientAction{ReplaceMarkedText{"9"}});
  EXPECT_TRUE(SelectingAt(12, 5).HandleEvent(DigitEv(7, split)).actions.empty());
  // A split that does not fit the list is read as nine per page.
  EXPECT_EQ(
      SelectingAt(12, 2).HandleEvent(PageEv(UserAction::Down, {0, 4, 12})).next.selected_index(),
      11u);
}

TEST(InputStateTest, SelectingCancelDiscardsInputAndCandidateCacheWithoutObservation) {
  for (const bool live : {false, true}) {
    SCOPED_TRACE(live);
    const HandleResult cancelled =
        Selecting().WithLiveConversion(live).HandleEvent(Ev(UserAction::Cancel));
    EXPECT_EQ(cancelled.next.kind(), K::Idle);
    EXPECT_EQ(cancelled.next.Reading(), "");
    EXPECT_TRUE(cancelled.next.candidates().empty());
    EXPECT_EQ(cancelled.actions, (Actions{HideCandidateWindow{}, CancelMarkedText{}}));
  }
}

// DEV-1735: typing while Selecting commits the highlight like Enter (and is
// observed like it), then the key starts a new reading as if typed from Idle.
TEST(InputStateTest, TypingWhileSelectingCommitsTheHighlightAndStartsANewReading) {
  const InputState selecting = Selecting().HandleEvent(Ev(UserAction::Down)).next;
  const HandleResult result = selecting.HandleEvent(Ev(UserAction::Input, U'k'));
  EXPECT_EQ(result.next.kind(), K::Composing);
  EXPECT_EQ(result.next.Reading(), "k");
  EXPECT_TRUE(result.next.candidates().empty());
  EXPECT_EQ(result.next.selected_index(), 0u);
  EXPECT_EQ(result.actions,
            (Actions{HideCandidateWindow{}, ReplaceMarkedText{"かな"}, CommitMarkedText{},
                     ObserveCommit{"かな", "かな"}, ReplaceMarkedText{"k"}, QueryCandidates{"k"}}));
}

TEST(InputStateTest, TypingWhileSelectingWithLiveConversionStartsPreviewing) {
  const HandleResult result =
      Selecting().WithLiveConversion(true).HandleEvent(Ev(UserAction::Input, U'k'));
  EXPECT_EQ(result.next.kind(), K::Previewing);
  EXPECT_EQ(result.next.Reading(), "k");
  EXPECT_EQ(result.actions,
            (Actions{HideCandidateWindow{}, ReplaceMarkedText{"仮名"}, CommitMarkedText{},
                     ObserveCommit{"かな", "仮名"}, ReplaceMarkedText{"k"}, QueryCandidates{"k"}}));
}

TEST(InputStateTest, SymbolsLongVowelAndAlnumWhileSelectingAlsoCommitFirst) {
  const struct {
    UserActionEvent event;
    const char* reading;
  } cases[] = {
      {Ev(UserAction::Input, U'-'), "ー"},     {Ev(UserAction::Input, U'/'), "/"},
      {Ev(UserAction::Input, U'、'), "、"},    {Ev(UserAction::Input, U'。'), "。"},
      {Ev(UserAction::InputAlnum, U'A'), "A"},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.reading);
    const HandleResult result = Selecting().HandleEvent(c.event);
    EXPECT_EQ(result.next.kind(), K::Composing);
    EXPECT_EQ(result.next.Reading(), c.reading);
    EXPECT_EQ(result.actions, (Actions{HideCandidateWindow{}, ReplaceMarkedText{"仮名"},
                                       CommitMarkedText{}, ObserveCommit{"かな", "仮名"},
                                       ReplaceMarkedText{c.reading}, QueryCandidates{c.reading}}));
  }
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

TEST(InputStateTest, CorrectedReadingPreservesComposingAndPreviewingState) {
  for (const bool live : {false, true}) {
    SCOPED_TRACE(live);
    const InputState state = Feed(InputState{}.WithLiveConversion(live), U"kanak")
                                 .HandleCandidatesArrived(KanaCandidates())
                                 .next;
    ASSERT_TRUE(state.pending_romaji().HasPending());
    const InputState corrected = state.WithCorrectedReading("かね");
    EXPECT_EQ(corrected.kind(), state.kind());
    EXPECT_EQ(corrected.confirmed_kana(), "かね");
    EXPECT_EQ(corrected.Reading(), "かね");
    EXPECT_FALSE(corrected.pending_romaji().HasPending());
    EXPECT_TRUE(SameCandidateList(corrected.candidates(), state.candidates()));
    EXPECT_EQ(corrected.selected_index(), state.selected_index());
    EXPECT_EQ(corrected.awaiting_candidates(), state.awaiting_candidates());
    EXPECT_EQ(corrected.live_conversion(), live);
    EXPECT_EQ(state.Reading(), "かなk");

    const HandleResult typed = corrected.HandleEvent(Ev(UserAction::Input, U'a'));
    EXPECT_EQ(typed.next.Reading(), "かねあ");
    EXPECT_EQ(typed.actions, (Actions{ReplaceMarkedText{"かねあ"}, QueryCandidates{"かねあ"}}));
  }
}

TEST(InputStateTest, CorrectedReadingPreservesSelectingSnapshotAndSelection) {
  const std::vector<Candidate> candidates = {Candidate{"金", "かね"}, Candidate{"鐘", "かね"}};
  const InputState state = Composing()
                               .HandleEvent(Ev(UserAction::StartConversion))
                               .next.HandleCandidatesArrived(candidates)
                               .next.HandleEvent(Ev(UserAction::NextCandidate))
                               .next.WithLiveConversion(true);
  ASSERT_EQ(state.selected_index(), 1u);
  const InputState corrected = state.WithCorrectedReading("かね");
  EXPECT_EQ(corrected.kind(), K::Selecting);
  EXPECT_EQ(corrected.Reading(), "かね");
  EXPECT_TRUE(SameCandidateList(corrected.candidates(), candidates));
  EXPECT_EQ(corrected.selected_index(), 1u);
  EXPECT_FALSE(corrected.awaiting_candidates());
  EXPECT_TRUE(corrected.live_conversion());
  EXPECT_EQ(state.Reading(), "かな");

  EXPECT_EQ(corrected.HandleEvent(Ev(UserAction::Cancel)).actions,
            (Actions{HideCandidateWindow{}, CancelMarkedText{}}));
  EXPECT_EQ(corrected.HandleEvent(Ev(UserAction::Commit)).actions,
            (Actions{HideCandidateWindow{}, ReplaceMarkedText{"鐘"}, CommitMarkedText{},
                     ObserveCommit{"かね", "鐘"}}));
}

TEST(InputStateTest, CorrectedReadingKeepsAwaitingCandidates) {
  const InputState state = Composing().HandleEvent(Ev(UserAction::StartConversion)).next;
  ASSERT_TRUE(state.awaiting_candidates());
  const InputState corrected = state.WithCorrectedReading("かね");
  EXPECT_EQ(corrected.kind(), K::Composing);
  EXPECT_EQ(corrected.Reading(), "かね");
  EXPECT_TRUE(corrected.awaiting_candidates());
  const HandleResult arrived = corrected.HandleCandidatesArrived({Candidate{"金", "かね"}});
  EXPECT_EQ(arrived.next.kind(), K::Selecting);
  EXPECT_FALSE(arrived.next.awaiting_candidates());
}

TEST(InputStateTest, CorrectedReadingKeepsCustomRomajiTable) {
  RomajiKanaConverter romaji;
  romaji.SetCustomTable(CustomRomajiLoader::Parse("z.\t…\n").table);
  ASSERT_TRUE(romaji.Feed('z').empty());
  const InputState corrected =
      InputState{}.WithComposition("かな", romaji).WithCorrectedReading("かね");
  EXPECT_FALSE(corrected.pending_romaji().HasPending());
  EXPECT_TRUE(corrected.pending_romaji().HasCustomTable());
  EXPECT_EQ(Feed(corrected, U"z.").Reading(), "かね…");
}

TEST(InputStateTest, EmptyCorrectedReadingLeavesActiveStateUnchanged) {
  for (const InputState& state : {Feed(Composing(), U"k"), Feed(Previewing(), U"k"),
                                  Selecting().HandleEvent(Ev(UserAction::NextCandidate)).next,
                                  Composing().HandleEvent(Ev(UserAction::StartConversion)).next}) {
    SCOPED_TRACE(static_cast<int>(state.kind()));
    const InputState corrected = state.WithCorrectedReading("");
    EXPECT_EQ(corrected.kind(), state.kind());
    EXPECT_EQ(corrected.confirmed_kana(), state.confirmed_kana());
    EXPECT_EQ(corrected.Reading(), state.Reading());
    EXPECT_EQ(corrected.pending_romaji().PreviewPending(), state.pending_romaji().PreviewPending());
    EXPECT_TRUE(SameCandidateList(corrected.candidates(), state.candidates()));
    EXPECT_EQ(corrected.selected_index(), state.selected_index());
    EXPECT_EQ(corrected.awaiting_candidates(), state.awaiting_candidates());
    EXPECT_EQ(corrected.live_conversion(), state.live_conversion());
  }
}

TEST(InputStateTest, CorrectedReadingIgnoresInactiveStates) {
  for (const InputState& state : {InputState{}, UnicodeInput()}) {
    SCOPED_TRACE(static_cast<int>(state.kind()));
    const InputState corrected = state.WithCorrectedReading("かね");
    EXPECT_EQ(corrected.kind(), state.kind());
    EXPECT_EQ(corrected.Reading(), state.Reading());
    EXPECT_EQ(corrected.unicode_hex(), state.unicode_hex());
  }
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

// StartConversion never enters Selecting without a snapshot of a window query.
TEST(InputStateTest, CacheMissQueriesAndStaysComposing) {
  const HandleResult result = Composing().HandleEvent(Ev(UserAction::StartConversion));
  EXPECT_EQ(result.next.kind(), K::Composing);
  EXPECT_TRUE(result.next.awaiting_candidates());
  EXPECT_EQ(result.actions, (Actions{QueryCandidates{"かな", true}}));
}

// DEV-1653: the per-key live results cached while typing never open the window.
TEST(InputStateTest, StartConversionIgnoresThePerKeyCacheAndQueriesForTheWindow) {
  for (const InputState& typed : {Composing(), Previewing()}) {
    SCOPED_TRACE(static_cast<int>(typed.kind()));
    const InputState cached = typed.HandleCandidatesArrived(KanaCandidates()).next;
    ASSERT_EQ(cached.candidates().size(), 3u);
    for (const UserAction action :
         {UserAction::StartConversion, UserAction::NextCandidate, UserAction::PrevCandidate}) {
      const HandleResult result = cached.HandleEvent(Ev(action));
      EXPECT_EQ(result.next.kind(), typed.kind());
      EXPECT_TRUE(result.next.awaiting_candidates());
      EXPECT_TRUE(result.next.candidates().empty());
      EXPECT_EQ(result.actions, (Actions{QueryCandidates{"かな", true}}));
    }
  }
}

// While the window is awaited a digit is typed into the reading by the map;
// the TIP holds it for the window instead (legacy-parity-spec §1.2).
TEST(InputStateTest, CacheMissEnterCommitsAsIsAndDigitsAreInput) {
  const InputState waiting = Composing().HandleEvent(Ev(UserAction::StartConversion)).next;
  const auto digit = MapUserAction('1', 0, waiting.kind());
  ASSERT_TRUE(digit.has_value());
  EXPECT_EQ(digit->action, UserAction::Input);

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

// DEV-1813 (legacy-parity-spec §1.2): the TIP holds the keys typed while the
// window is awaited and replays them once the candidates arrive, so core sees
// them in Selecting and the result is the synchronous legacy one.
TEST(InputStateTest, KeysReplayedAfterTheAnswerActInSelecting) {
  const InputState waiting = Composing().HandleEvent(Ev(UserAction::StartConversion)).next;
  ASSERT_TRUE(waiting.awaiting_candidates());
  const InputState opened = waiting.HandleCandidatesArrived(KanaCandidates()).next;
  ASSERT_EQ(opened.kind(), K::Selecting);

  // Space, Space, Enter: the second Space is NextCandidate in Selecting.
  const auto space = MapUserAction(vk::kSpace, 0, opened.kind());
  ASSERT_TRUE(space.has_value());
  const InputState second = opened.HandleEvent(*space).next;
  const HandleResult enter = second.HandleEvent(Ev(UserAction::Commit));
  EXPECT_EQ(enter.next.kind(), K::Idle);
  EXPECT_EQ(enter.actions, (Actions{HideCandidateWindow{}, ReplaceMarkedText{"かな"},
                                    CommitMarkedText{}, ObserveCommit{"かな", "かな"}}));

  // Space, 3: the digit selects in Selecting.
  const auto digit = MapUserAction(vk::k0 + 3, 0, opened.kind());
  ASSERT_TRUE(digit.has_value());
  const HandleResult selected = opened.HandleEvent(*digit);
  EXPECT_EQ(selected.next.kind(), K::Idle);
  EXPECT_EQ(selected.actions, (Actions{HideCandidateWindow{}, ReplaceMarkedText{"カナ"},
                                       CommitMarkedText{}, ObserveCommit{"かな", "カナ"}}));
}

// With no candidate the window never opens: the replayed Enter commits the
// reading as typed.
TEST(InputStateTest, EnterReplayedAfterAnEmptyAnswerCommitsTheReading) {
  const InputState waiting = Composing().HandleEvent(Ev(UserAction::StartConversion)).next;
  const InputState stopped = waiting.HandleCandidatesArrived({}).next;
  ASSERT_EQ(stopped.kind(), K::Composing);
  const HandleResult committed = stopped.HandleEvent(Ev(UserAction::Commit));
  EXPECT_EQ(committed.next.kind(), K::Idle);
  EXPECT_EQ(committed.actions, (Actions{ReplaceMarkedText{"かな"}, CommitMarkedText{}}));
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
  EXPECT_EQ(result.actions, (Actions{ReplaceMarkedText{"かん"}, QueryCandidates{"かん", true}}));
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

// DEV-1748: an apostrophe after a pending n / nn ends ん and is consumed.
TEST(InputStateTest, ApostropheAfterNSeparatesN) {
  EXPECT_EQ(Feed(InputState{}, U"gen'inn").Reading(), "げんいん");
  EXPECT_EQ(Feed(InputState{}, U"kann'a").Reading(), "かんあ");
  EXPECT_EQ(Feed(InputState{}, U"ka'i").Reading(), "か'い");

  RomajiKanaConverter romaji;
  romaji.SetCustomTable(CustomRomajiLoader::Parse("n\tん\t1\na\tあ\n").table);
  const auto custom = Feed(InputState{}.WithComposition("", romaji), U"n'a");
  EXPECT_EQ(custom.Reading(), "ん'あ");
}

TEST(InputStateTest, CustomAsciiPunctuationUsesPendingRule) {
  RomajiKanaConverter romaji;
  romaji.SetCustomTable(CustomRomajiLoader::Parse("z.\t…\n").table);
  const auto state = Feed(InputState{}.WithComposition("", romaji), U"z.");
  EXPECT_EQ(state.Reading(), "…");
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
  }
}

// DEV-1811 (tsf-deep-integration-spec §3.5): digits and the full-width symbols
// the TIP resolves join the reading as typed; a pending romaji is flushed first.
TEST(InputStateTest, DigitsAndFullWidthSymbolsJoinTheReading) {
  InputState state;
  for (const char32_t cp : {U'd', U'a', U'i', U'3', U'k', U'a', U'i'})
    state = state.HandleEvent(Ev(UserAction::Input, cp)).next;
  EXPECT_EQ(state.Reading(), "だい3かい");

  state = InputState{};
  for (const char32_t cp : {U'j', U'i', U'：', U'k', U'a', U'n'})
    state = state.HandleEvent(Ev(UserAction::Input, cp)).next;
  EXPECT_EQ(state.Reading(), "じ：かん");

  // A symbol starts a reading from Idle; `-` is the long vowel mark.
  const HandleResult bracket = InputState{}.HandleEvent(Ev(UserAction::Input, U'「'));
  EXPECT_EQ(bracket.next.kind(), K::Composing);
  EXPECT_EQ(bracket.next.Reading(), "「");
  EXPECT_EQ(InputState{}.HandleEvent(Ev(UserAction::Input, U'-')).next.Reading(), "ー");
  // A pending "k" is flushed before the symbol.
  state = InputState{}.HandleEvent(Ev(UserAction::Input, U'k')).next;
  EXPECT_EQ(state.HandleEvent(Ev(UserAction::Input, U'＠')).next.Reading(), "k＠");
}

// Selecting: 0 commits the highlight like Enter and starts the next reading.
TEST(InputStateTest, ZeroWhileSelectingCommitsAndStartsTheNextReading) {
  const auto zero = MapUserAction('0', 0, K::Selecting);
  ASSERT_TRUE(zero.has_value());
  ASSERT_EQ(zero->action, UserAction::Input);
  const HandleResult result = Selecting().HandleEvent(Ev(UserAction::Input, U'0'));
  EXPECT_EQ(result.next.kind(), K::Composing);
  EXPECT_EQ(result.next.Reading(), "0");
  EXPECT_EQ(result.actions,
            (Actions{HideCandidateWindow{}, ReplaceMarkedText{"仮名"}, CommitMarkedText{},
                     ObserveCommit{"かな", "仮名"}, ReplaceMarkedText{"0"}, QueryCandidates{"0"}}));
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

// commit-undo-spec §5: the TIP has restored the reading into the composition
// and passes it with UndoCommit; the state machine only enters Composing
// (Previewing with live conversion) and asks for the per-key candidates.
TEST(InputStateTest, UndoCommitEntersComposingWithTheRestoredReading) {
  UserActionEvent undo = Ev(UserAction::UndoCommit);
  undo.reading = "かな";
  const HandleResult result = InputState{}.HandleEvent(undo);
  EXPECT_EQ(result.next.kind(), K::Composing);
  EXPECT_EQ(result.next.Reading(), "かな");
  EXPECT_EQ(result.next.confirmed_kana(), "かな");
  EXPECT_FALSE(result.next.pending_romaji().HasPending());
  // No ReplaceMarkedText: the edit session already wrote the composition.
  EXPECT_EQ(result.actions, (Actions{QueryCandidates{"かな"}}));
}

TEST(InputStateTest, UndoCommitEntersPreviewingWithLiveConversion) {
  UserActionEvent undo = Ev(UserAction::UndoCommit);
  undo.reading = "かな";
  const HandleResult result = InputState{}.WithLiveConversion(true).HandleEvent(undo);
  EXPECT_EQ(result.next.kind(), K::Previewing);
  EXPECT_EQ(result.next.Reading(), "かな");
  EXPECT_EQ(result.actions, (Actions{QueryCandidates{"かな"}}));
}

TEST(InputStateTest, UndoCommitWithoutAReadingStaysIdle) {
  const HandleResult result = InputState{}.HandleEvent(Ev(UserAction::UndoCommit));
  EXPECT_EQ(result.next.kind(), K::Idle);
  EXPECT_TRUE(result.actions.empty());
}

TEST(InputStateTest, UndoCommitOutsideIdleLeavesTheStateAlone) {
  UserActionEvent undo = Ev(UserAction::UndoCommit);
  undo.reading = "べつ";
  for (InputState (*make)() : {&Composing, &Previewing, &Selecting, &UnicodeInput}) {
    const InputState state = make();
    const HandleResult result = state.HandleEvent(undo);
    EXPECT_EQ(result.next.kind(), state.kind());
    EXPECT_EQ(result.next.Reading(), state.Reading());
    EXPECT_EQ(result.next.unicode_hex(), state.unicode_hex());
    EXPECT_TRUE(result.actions.empty());
  }
}

// After the undo the keys work as while typing a reading (spec §5).
TEST(InputStateTest, ReadingRestoredByUndoCommitEditsLikeATypedReading) {
  UserActionEvent undo = Ev(UserAction::UndoCommit);
  undo.reading = "かな";
  const InputState restored = InputState{}.HandleEvent(undo).next;

  const HandleResult typed = restored.HandleEvent(Ev(UserAction::Input, U'k'));
  EXPECT_EQ(typed.next.Reading(), "かなk");
  EXPECT_EQ(typed.actions, (Actions{ReplaceMarkedText{"かなk"}, QueryCandidates{"かなk"}}));

  const HandleResult erased = restored.HandleEvent(Ev(UserAction::Backspace));
  EXPECT_EQ(erased.next.Reading(), "か");
  EXPECT_EQ(erased.actions, (Actions{ReplaceMarkedText{"か"}, QueryCandidates{"か"}}));

  const HandleResult converting = restored.HandleEvent(Ev(UserAction::StartConversion));
  EXPECT_EQ(converting.next.kind(), K::Composing);
  EXPECT_TRUE(converting.next.awaiting_candidates());
  EXPECT_EQ(converting.actions, (Actions{QueryCandidates{"かな", true}}));
  const InputState selecting = converting.next.HandleCandidatesArrived(KanaCandidates()).next;
  EXPECT_EQ(selecting.kind(), K::Selecting);

  const HandleResult canceled = restored.HandleEvent(Ev(UserAction::Cancel));
  EXPECT_EQ(canceled.next.kind(), K::Idle);
  EXPECT_EQ(canceled.actions, (Actions{CancelMarkedText{}}));

  // Enter commits the reading as it stands (legacy-parity-spec §1.3).
  const HandleResult committed = restored.HandleEvent(Ev(UserAction::Commit));
  EXPECT_EQ(committed.next.kind(), K::Idle);
}

// The chord reaches UndoCommit only from Idle (spec §3), so the ordinary
// Ctrl+Backspace of a reading or a candidate window is not an undo.
TEST(InputStateTest, UndoCommitKeyChordHasNoEffectWhileTyping) {
  for (const K kind : {K::Composing, K::Previewing, K::Selecting, K::UnicodeInput}) {
    EXPECT_FALSE(MapUserAction(vk::kBack, kModifierCtrl, kind).has_value());
  }
  const auto idle = MapUserAction(vk::kBack, kModifierCtrl, K::Idle);
  ASSERT_TRUE(idle.has_value());
  EXPECT_EQ(idle->action, UserAction::UndoCommit);
  EXPECT_EQ(MapUserAction(vk::kBack, kModifierCtrl | kModifierShift, K::Idle)->action,
            UserAction::Forget);
}

// tsf-deep-integration-spec §3.6 (DEV-1812): the TIP commits F6-F10 itself
// (core::BuildCommitForm) on every input path and then resets this state, so
// the state machine leaves the reading untouched and emits nothing for them.
TEST(InputStateTest, CommitFormActionsAreLeftToTheFrontend) {
  for (const UserAction action :
       {UserAction::CommitHiragana, UserAction::CommitKatakana, UserAction::CommitHalfwidthKatakana,
        UserAction::CommitFullwidthRoman, UserAction::CommitHalfwidthRoman}) {
    for (InputState (*make)() : {&Composing, &Previewing, &Selecting, &UnicodeInput}) {
      const InputState state = make();
      const HandleResult result = state.HandleEvent(Ev(action));
      EXPECT_TRUE(result.actions.empty());
      EXPECT_EQ(result.next.kind(), state.kind());
      EXPECT_EQ(result.next.Reading(), state.Reading());
    }
  }
}

TEST(InputStateTest, LiveConversionTurnsComposingIntoPreviewing) {
  const InputState first =
      InputState{}.WithLiveConversion(true).HandleEvent(Ev(UserAction::Input, U'k')).next;
  EXPECT_EQ(first.kind(), K::Previewing);
  EXPECT_EQ(first.HandleEvent(Ev(UserAction::Input, U'a')).next.kind(), K::Previewing);
}

TEST(InputStateTest, PreviewingStartConversionWaitsForTheWindowQueryLikeComposing) {
  const HandleResult waiting = Previewing().HandleEvent(Ev(UserAction::StartConversion));
  EXPECT_EQ(waiting.next.kind(), K::Previewing);
  EXPECT_TRUE(waiting.next.awaiting_candidates());
  EXPECT_EQ(waiting.actions, (Actions{QueryCandidates{"かな", true}}));
  EXPECT_EQ(waiting.next.HandleCandidatesArrived(KanaCandidates()).next.kind(), K::Selecting);
}

// legacy-parity-spec §4.1 (DEV-1963): both double taps open the same prompt
// window with a different initial prompt, and the document stays Idle because
// the window owns its own keys. A reading in progress is left to the TIP's key
// handling, so the double taps do nothing there.
TEST(InputStateTest, DoubleTapsAskForThePromptWindowAndStayIdle) {
  const HandleResult alnum = InputState{}.HandleEvent(Ev(UserAction::StartAlnumDouble));
  EXPECT_EQ(alnum.next.kind(), K::Idle);
  EXPECT_EQ(alnum.actions, (Actions{ShowReplaceSuggestionPrompt{PromptPreset::English}}));
  const HandleResult kana = InputState{}.HandleEvent(Ev(UserAction::StartKanaDouble));
  EXPECT_EQ(kana.next.kind(), K::Idle);
  EXPECT_EQ(kana.actions, (Actions{ShowReplaceSuggestionPrompt{PromptPreset::Japanese}}));
  for (const UserAction action : {UserAction::StartAlnumDouble, UserAction::StartKanaDouble}) {
    const HandleResult composing = Composing().HandleEvent(Ev(action));
    EXPECT_TRUE(composing.actions.empty());
    EXPECT_EQ(composing.next.Reading(), "かな");
  }
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

// M18 acceptance: Ctrl+Shift+U, 30A1, Enter commits 「ァ」 (U+30A1, UTF-8 E3 82 A1).
TEST(InputStateTest, CtrlShiftUThenHexAndEnterCommitsSmallKatakanaA) {
  const auto start = MapUserAction('U', kModifierCtrl | kModifierShift, K::Idle);
  ASSERT_TRUE(start.has_value());
  EXPECT_EQ(start->action, UserAction::StartUnicodeInput);
  const InputState typed = Feed(InputState{}.HandleEvent(*start).next, U"30A1");
  EXPECT_EQ(typed.unicode_hex(), "30A1");

  const auto enter = MapUserAction(vk::kReturn, 0, typed.kind());
  ASSERT_TRUE(enter.has_value());
  EXPECT_EQ(enter->action, UserAction::Commit);
  const HandleResult committed = typed.HandleEvent(*enter);
  EXPECT_EQ(committed.next.kind(), K::Idle);
  EXPECT_EQ(committed.actions, (Actions{ReplaceMarkedText{"\xE3\x82\xA1"}, CommitMarkedText{}}));
}

TEST(InputStateTest, StartUnicodeInputFromCompositionCommitsReadingFirst) {
  const HandleResult result = Composing().HandleEvent(Ev(UserAction::StartUnicodeInput));
  EXPECT_EQ(result.next.kind(), K::UnicodeInput);
  EXPECT_EQ(result.actions,
            (Actions{ReplaceMarkedText{"かな"}, CommitMarkedText{}, ReplaceMarkedText{"U+"}}));
}

// ---- M66 (docs/segment-editing-spec.md): the first segment ----

// きょうは is four code points; the small ょ counts as one (spec §4).
std::vector<Candidate> WholeCandidates() {
  return {Candidate{"今日は", "きょうは"}, Candidate{"きょうは", "きょうは"}};
}

std::vector<Candidate> KyouCandidates() {
  return {Candidate{"今日", "きょう"}, Candidate{"京", "きょう"}, Candidate{"きょう", "きょう"}};
}

InputState SelectingKyouha(bool live = false) {
  const auto waiting = Feed(InputState{}.WithLiveConversion(live), U"kyouha")
                           .HandleEvent(Ev(UserAction::StartConversion))
                           .next;
  return waiting.HandleCandidatesArrived(WholeCandidates()).next;
}

// The first segment shrunk to きょう with its candidates delivered.
InputState ResizedKyou(bool live = false) {
  const auto waiting = SelectingKyouha(live).HandleEvent(Ev(UserAction::ShrinkSegment)).next;
  return waiting.HandleSegmentCandidatesArrived("きょう", KyouCandidates()).next;
}

TEST(SegmentEditingTest, SelectingStartsWithTheWholeReadingAsTheFirstSegment) {
  const InputState selecting = SelectingKyouha();
  EXPECT_EQ(selecting.segment_end(), 4u);
  EXPECT_FALSE(selecting.resizing_segment());
  EXPECT_EQ(selecting.FirstSegmentReading(), "きょうは");
  EXPECT_EQ(selecting.RemainingReading(), "");
  EXPECT_FALSE(selecting.awaiting_segment_candidates());
}

TEST(SegmentEditingTest, ShrinkAsksForTheFirstSegmentsCandidatesAndDrawsItAsTheReading) {
  const HandleResult shrunk = SelectingKyouha().HandleEvent(Ev(UserAction::ShrinkSegment));
  EXPECT_EQ(shrunk.next.kind(), K::Selecting);
  EXPECT_EQ(shrunk.next.segment_end(), 3u);
  EXPECT_TRUE(shrunk.next.resizing_segment());
  EXPECT_TRUE(shrunk.next.awaiting_segment_candidates());
  EXPECT_EQ(shrunk.next.FirstSegmentReading(), "きょう");
  EXPECT_EQ(shrunk.next.RemainingReading(), "は");
  // The window stays with the previous list; no HideCandidateWindow.
  EXPECT_TRUE(SameCandidateList(shrunk.next.candidates(), WholeCandidates()));
  EXPECT_EQ(shrunk.actions,
            (Actions{ReplaceMarkedText{"きょうは"}, QueryCandidates{"きょう", true, "は"}}));
}

TEST(SegmentEditingTest, EveryShrinkBelowTheWholeReadingQueriesAgainAndStopsAtOneCodePoint) {
  InputState state = SelectingKyouha();
  const struct {
    size_t end;
    const char* first;
    const char* rest;
  } steps[] = {{3, "きょう", "は"}, {2, "きょ", "うは"}, {1, "き", "ょうは"}};
  for (const auto& step : steps) {
    SCOPED_TRACE(step.end);
    const HandleResult shrunk = state.HandleEvent(Ev(UserAction::ShrinkSegment));
    EXPECT_EQ(shrunk.next.segment_end(), step.end);
    EXPECT_EQ(shrunk.actions, (Actions{ReplaceMarkedText{"きょうは"},
                                       QueryCandidates{step.first, true, step.rest}}));
    state = shrunk.next;
  }
  // e = 1: consumed without a change.
  const HandleResult stopped = state.HandleEvent(Ev(UserAction::ShrinkSegment));
  EXPECT_TRUE(stopped.actions.empty());
  EXPECT_EQ(stopped.next.segment_end(), 1u);
}

TEST(SegmentEditingTest, ExtendBeforeAnyResizeStartsFromOneCodePoint) {
  const HandleResult extended = SelectingKyouha().HandleEvent(Ev(UserAction::ExtendSegment));
  EXPECT_EQ(extended.next.segment_end(), 1u);
  EXPECT_EQ(extended.actions,
            (Actions{ReplaceMarkedText{"きょうは"}, QueryCandidates{"き", true, "ょうは"}}));
}

TEST(SegmentEditingTest, ExtendingBackToTheWholeReadingRestoresItsListWithoutAQuery) {
  const InputState resized = ResizedKyou().HandleEvent(Ev(UserAction::Down)).next;
  const HandleResult whole = resized.HandleEvent(Ev(UserAction::ExtendSegment));
  EXPECT_FALSE(whole.next.resizing_segment());
  EXPECT_EQ(whole.next.segment_end(), 4u);
  EXPECT_EQ(whole.next.selected_index(), 0u);
  EXPECT_TRUE(SameCandidateList(whole.next.candidates(), WholeCandidates()));
  EXPECT_EQ(whole.actions,
            (Actions{ShowCandidateWindow{WholeCandidates(), 0}, ReplaceMarkedText{"今日は"}}));
  // After a resize Shift+→ stops at the whole reading; Shift+← resizes again.
  const HandleResult stopped = whole.next.HandleEvent(Ev(UserAction::ExtendSegment));
  EXPECT_TRUE(stopped.actions.empty());
  EXPECT_FALSE(stopped.next.resizing_segment());
  EXPECT_EQ(whole.next.HandleEvent(Ev(UserAction::ShrinkSegment)).next.segment_end(), 3u);
}

TEST(SegmentEditingTest, ExtendWhileResizedGrowsByOneCodePoint) {
  const InputState one = SelectingKyouha().HandleEvent(Ev(UserAction::ExtendSegment)).next;
  const HandleResult two = one.HandleEvent(Ev(UserAction::ExtendSegment));
  EXPECT_EQ(two.next.segment_end(), 2u);
  EXPECT_EQ(two.actions,
            (Actions{ReplaceMarkedText{"きょうは"}, QueryCandidates{"きょ", true, "うは"}}));
}

TEST(SegmentEditingTest, AOneCodePointReadingConsumesBothResizeKeys) {
  const InputState waiting =
      Feed(InputState{}, U"ka").HandleEvent(Ev(UserAction::StartConversion)).next;
  const InputState single = waiting.HandleCandidatesArrived({Candidate{"蚊", "か"}}).next;
  ASSERT_EQ(single.kind(), K::Selecting);
  ASSERT_EQ(single.segment_end(), 1u);
  for (const UserAction action : {UserAction::ShrinkSegment, UserAction::ExtendSegment}) {
    const HandleResult result = single.HandleEvent(Ev(action));
    EXPECT_TRUE(result.actions.empty());
    EXPECT_EQ(result.next.kind(), K::Selecting);
    EXPECT_FALSE(result.next.resizing_segment());
  }
}

// Spec §5.2: only the answer for the current first segment replaces the list.
TEST(SegmentEditingTest, TheFirstSegmentsAnswerReplacesTheListAndAnEarlierOneIsDropped) {
  const InputState waiting = SelectingKyouha().HandleEvent(Ev(UserAction::ShrinkSegment)).next;
  const HandleResult arrived = waiting.HandleSegmentCandidatesArrived("きょう", KyouCandidates());
  EXPECT_EQ(arrived.next.kind(), K::Selecting);
  EXPECT_FALSE(arrived.next.awaiting_segment_candidates());
  EXPECT_EQ(arrived.next.selected_index(), 0u);
  EXPECT_TRUE(SameCandidateList(arrived.next.candidates(), KyouCandidates()));
  EXPECT_EQ(arrived.actions,
            (Actions{ShowCandidateWindow{KyouCandidates(), 0}, ReplaceMarkedText{"今日は"}}));

  // Shrunk again before the answer: the answer for きょう is the earlier end's.
  const InputState again = waiting.HandleEvent(Ev(UserAction::ShrinkSegment)).next;
  const HandleResult stale = again.HandleSegmentCandidatesArrived("きょう", KyouCandidates());
  EXPECT_TRUE(stale.actions.empty());
  EXPECT_TRUE(stale.next.awaiting_segment_candidates());
  EXPECT_TRUE(SameCandidateList(stale.next.candidates(), WholeCandidates()));
  // An empty answer and a second one change nothing either.
  EXPECT_TRUE(
      waiting.HandleSegmentCandidatesArrived("きょう", {}).next.awaiting_segment_candidates());
  EXPECT_TRUE(arrived.next.HandleSegmentCandidatesArrived("きょう", {Candidate{"興", "きょう"}})
                  .actions.empty());
  // The whole reading's feedback never replaces the first segment's list,
  // neither through the whole reading's entry nor as an answer for きょうは.
  EXPECT_TRUE(waiting.HandleCandidatesArrived(KyouCandidates()).actions.empty());
  const HandleResult whole_answer =
      waiting.HandleSegmentCandidatesArrived("きょうは", WholeCandidates());
  EXPECT_TRUE(whole_answer.actions.empty());
  EXPECT_TRUE(whole_answer.next.awaiting_segment_candidates());
  EXPECT_EQ(whole_answer.next.segment_end(), 3u);
  EXPECT_TRUE(SelectingKyouha()
                  .HandleSegmentCandidatesArrived("きょうは", WholeCandidates())
                  .actions.empty());
}

TEST(SegmentEditingTest, KeysWhileAwaitingTheFirstSegmentAreLeftForTheFrontendToReplay) {
  const InputState waiting = SelectingKyouha().HandleEvent(Ev(UserAction::ShrinkSegment)).next;
  for (const UserActionEvent& event :
       {Ev(UserAction::Commit), Ev(UserAction::Forward), Ev(UserAction::Backward),
        Ev(UserAction::Backspace), Ev(UserAction::NextCandidate), Ev(UserAction::Input, U'k'),
        Ev(UserAction::SelectByDigit, 0, 1), Ev(UserAction::StartUnicodeInput),
        Ev(UserAction::CommitKatakana), PageEv(UserAction::Down)}) {
    SCOPED_TRACE(static_cast<int>(event.action));
    const HandleResult result = waiting.HandleEvent(event);
    EXPECT_TRUE(result.actions.empty());
    EXPECT_EQ(result.next.kind(), K::Selecting);
    EXPECT_TRUE(result.next.awaiting_segment_candidates());
    EXPECT_EQ(result.next.segment_end(), 3u);
  }
  // The keys every state handles alike act as with the whole reading and
  // keep the wait.
  for (const UserAction action : {UserAction::ToggleHankaku, UserAction::ToggleHiraKata,
                                  UserAction::Forget, UserAction::ToggleDebugWindow}) {
    SCOPED_TRACE(static_cast<int>(action));
    const HandleResult result = waiting.HandleEvent(Ev(action));
    EXPECT_EQ(result.actions, SelectingKyouha().HandleEvent(Ev(action)).actions);
    EXPECT_EQ(result.actions.size(), 1u);
    EXPECT_TRUE(result.next.awaiting_segment_candidates());
    EXPECT_EQ(result.next.segment_end(), 3u);
  }
  const HandleResult canceled = waiting.HandleEvent(Ev(UserAction::Cancel));
  EXPECT_EQ(canceled.next.kind(), K::Idle);
  EXPECT_FALSE(canceled.next.awaiting_segment_candidates());
  EXPECT_EQ(canceled.actions, (Actions{HideCandidateWindow{}, CancelMarkedText{}}));
}

TEST(SegmentEditingTest, ExtendingBackToTheWholeReadingWhileAwaitingStopsWaiting) {
  const InputState waiting = SelectingKyouha().HandleEvent(Ev(UserAction::ShrinkSegment)).next;
  const HandleResult whole = waiting.HandleEvent(Ev(UserAction::ExtendSegment));
  EXPECT_FALSE(whole.next.awaiting_segment_candidates());
  EXPECT_FALSE(whole.next.resizing_segment());
  EXPECT_EQ(whole.actions,
            (Actions{ShowCandidateWindow{WholeCandidates(), 0}, ReplaceMarkedText{"今日は"}}));
  // The answer of the request the frontend cancels is dropped if it arrives.
  EXPECT_TRUE(
      whole.next.HandleSegmentCandidatesArrived("きょう", KyouCandidates()).actions.empty());
}

TEST(SegmentEditingTest, CandidateKeysMoveAmongTheFirstSegmentsCandidates) {
  const InputState resized = ResizedKyou();
  const HandleResult down = resized.HandleEvent(Ev(UserAction::Down));
  EXPECT_EQ(down.next.selected_index(), 1u);
  EXPECT_EQ(down.actions, (Actions{UpdateCandidateSelection{1}, ReplaceMarkedText{"京は"}}));
  EXPECT_EQ(resized.HandleEvent(Ev(UserAction::PrevCandidate)).actions,
            (Actions{UpdateCandidateSelection{2}, ReplaceMarkedText{"きょうは"}}));
  EXPECT_EQ(down.next.segment_end(), 3u);
}

// Spec §6.1: Enter, → (and Ctrl+F) and the digits.
TEST(SegmentEditingTest, PartialCommitCommitsTheFirstSegmentAndWaitsForTheRest) {
  const InputState second = ResizedKyou().HandleEvent(Ev(UserAction::Down)).next;
  for (const UserActionEvent& event :
       {Ev(UserAction::Commit), Ev(UserAction::Forward), Ev(UserAction::SelectByDigit, 0, 2)}) {
    SCOPED_TRACE(static_cast<int>(event.action));
    const HandleResult committed = second.HandleEvent(event);
    EXPECT_EQ(committed.next.kind(), K::Composing);
    EXPECT_EQ(committed.next.Reading(), "は");
    EXPECT_TRUE(committed.next.awaiting_candidates());
    EXPECT_FALSE(committed.next.resizing_segment());
    EXPECT_TRUE(committed.next.candidates().empty());
    EXPECT_EQ(committed.actions,
              (Actions{HideCandidateWindow{}, CommitFirstSegment{"京", "は"},
                       ObserveCommit{"きょう", "京"}, QueryCandidates{"は", true}}));
  }
  const HandleResult third = second.HandleEvent(Ev(UserAction::SelectByDigit, 0, 3));
  EXPECT_EQ(third.actions.at(1), (ClientAction{CommitFirstSegment{"きょう", "は"}}));
  // A digit past the list does nothing.
  EXPECT_TRUE(second.HandleEvent(Ev(UserAction::SelectByDigit, 0, 4)).actions.empty());
  // With live conversion the remaining reading waits in Previewing.
  EXPECT_EQ(ResizedKyou(true).HandleEvent(Ev(UserAction::Commit)).next.kind(), K::Previewing);
}

TEST(SegmentEditingTest, TheRemainingReadingsAnswerOpensSelectingWithItsWholeReading) {
  const InputState waiting = ResizedKyou().HandleEvent(Ev(UserAction::Commit)).next;
  const std::vector<Candidate> ha = {Candidate{"は", "は"}, Candidate{"葉", "は"}};
  const HandleResult opened = waiting.HandleCandidatesArrived(ha);
  EXPECT_EQ(opened.next.kind(), K::Selecting);
  EXPECT_FALSE(opened.next.resizing_segment());
  EXPECT_EQ(opened.next.segment_end(), 1u);
  EXPECT_EQ(opened.actions, (Actions{ShowCandidateWindow{ha, 0}, ReplaceMarkedText{"は"}}));
  // A one code point reading: Shift+→ cannot start over, Shift+← cannot shrink.
  EXPECT_TRUE(opened.next.HandleEvent(Ev(UserAction::ExtendSegment)).actions.empty());
  EXPECT_TRUE(opened.next.HandleEvent(Ev(UserAction::ShrinkSegment)).actions.empty());
  // Enter commits it whole, the M13 way.
  EXPECT_EQ(opened.next.HandleEvent(Ev(UserAction::Commit)).actions,
            (Actions{HideCandidateWindow{}, ReplaceMarkedText{"は"}, CommitMarkedText{},
                     ObserveCommit{"は", "は"}}));
}

// Spec §6.1: the frontend commits the first segment's reading in the form.
TEST(SegmentEditingTest, CharacterFormKeysWhileResizedAskForTheRemainingReading) {
  for (const UserAction action :
       {UserAction::CommitHiragana, UserAction::CommitKatakana, UserAction::CommitHalfwidthKatakana,
        UserAction::CommitFullwidthRoman, UserAction::CommitHalfwidthRoman}) {
    SCOPED_TRACE(static_cast<int>(action));
    const HandleResult result = ResizedKyou().HandleEvent(Ev(action));
    EXPECT_EQ(result.next.kind(), K::Composing);
    EXPECT_EQ(result.next.Reading(), "は");
    EXPECT_TRUE(result.next.awaiting_candidates());
    EXPECT_EQ(result.actions, (Actions{HideCandidateWindow{}, QueryCandidates{"は", true}}));
  }
}

TEST(SegmentEditingTest, BackspaceWhileResizedDropsTheSegmentAndEditsTheWholeReading) {
  const HandleResult result = ResizedKyou().HandleEvent(Ev(UserAction::Backspace));
  EXPECT_EQ(result.next.kind(), K::Composing);
  EXPECT_EQ(result.next.Reading(), "きょう");
  EXPECT_FALSE(result.next.resizing_segment());
  EXPECT_EQ(result.actions, (Actions{HideCandidateWindow{}, ReplaceMarkedText{"きょう"},
                                     QueryCandidates{"きょう"}}));
}

TEST(SegmentEditingTest, CancelWhileResizedCancelsTheWholeInput) {
  const HandleResult result = ResizedKyou().HandleEvent(Ev(UserAction::Cancel));
  EXPECT_EQ(result.next.kind(), K::Idle);
  EXPECT_EQ(result.next.Reading(), "");
  EXPECT_FALSE(result.next.resizing_segment());
  EXPECT_EQ(result.actions, (Actions{HideCandidateWindow{}, CancelMarkedText{}}));
}

// Spec §4, §8: typing, punctuation and Ctrl+Shift+U commit what is shown, the
// remaining reading as typed, and observe the first segment only.
TEST(SegmentEditingTest, TypingPunctuationAndUnicodeInputWhileResizedCommitTheShownText) {
  const HandleResult typed = ResizedKyou().HandleEvent(Ev(UserAction::Input, U'k'));
  EXPECT_EQ(typed.next.kind(), K::Composing);
  EXPECT_EQ(typed.next.Reading(), "k");
  EXPECT_EQ(typed.actions, (Actions{HideCandidateWindow{}, ReplaceMarkedText{"今日は"},
                                    CommitMarkedText{}, ObserveCommit{"きょう", "今日"},
                                    ReplaceMarkedText{"k"}, QueryCandidates{"k"}}));

  const HandleResult period =
      ResizedKyou().WithPunctuationConversion(true).HandleEvent(Ev(UserAction::Input, U'。'));
  EXPECT_EQ(period.next.kind(), K::Idle);
  EXPECT_EQ(period.actions, (Actions{HideCandidateWindow{}, ReplaceMarkedText{"今日は。"},
                                     CommitMarkedText{}, ObserveCommit{"きょう", "今日"}}));

  const HandleResult unicode = ResizedKyou().HandleEvent(Ev(UserAction::StartUnicodeInput));
  EXPECT_EQ(unicode.next.kind(), K::UnicodeInput);
  EXPECT_EQ(unicode.actions,
            (Actions{HideCandidateWindow{}, ReplaceMarkedText{"今日は"}, CommitMarkedText{},
                     ObserveCommit{"きょう", "今日"}, ReplaceMarkedText{"U+"}}));
}

// Spec §4: keys outside the table keep e and act as with the whole reading.
TEST(SegmentEditingTest, KeysOutsideTheTableKeepTheFirstSegment) {
  const InputState resized = ResizedKyou();
  for (const UserActionEvent& event :
       {Ev(UserAction::Delete), Ev(UserAction::LineHead), Ev(UserAction::LineEnd),
        Ev(UserAction::Backward), PageEv(UserAction::Down), PageEv(UserAction::Up),
        Ev(UserAction::Forget), Ev(UserAction::ToggleHankaku), Ev(UserAction::ToggleDebugWindow)}) {
    SCOPED_TRACE(static_cast<int>(event.action));
    const HandleResult result = resized.HandleEvent(event);
    EXPECT_EQ(result.next.kind(), K::Selecting);
    EXPECT_EQ(result.next.segment_end(), 3u);
    EXPECT_EQ(result.next.FirstSegmentReading(), "きょう");
    EXPECT_TRUE(SameCandidateList(result.next.candidates(), KyouCandidates()));
  }
  // ← is consumed without a change; one page has nowhere to move.
  EXPECT_TRUE(resized.HandleEvent(Ev(UserAction::Backward)).actions.empty());
  EXPECT_TRUE(resized.HandleEvent(PageEv(UserAction::Down)).actions.empty());
  EXPECT_EQ(resized.HandleEvent(Ev(UserAction::Forget)).actions, (Actions{ForgetLastCommit{}}));
}

// The whole reading after a resize round trip behaves as before M66.
TEST(SegmentEditingTest, TheWholeReadingAfterAResizeCommitsLikeM13) {
  const InputState whole = ResizedKyou().HandleEvent(Ev(UserAction::ExtendSegment)).next;
  EXPECT_EQ(whole.HandleEvent(Ev(UserAction::Commit)).actions,
            (Actions{HideCandidateWindow{}, ReplaceMarkedText{"今日は"}, CommitMarkedText{},
                     ObserveCommit{"きょうは", "今日は"}}));
  EXPECT_TRUE(whole.HandleEvent(Ev(UserAction::Forward)).actions.empty());
  EXPECT_TRUE(whole.HandleEvent(Ev(UserAction::CommitKatakana)).actions.empty());
  EXPECT_EQ(
      whole.HandleEvent(Ev(UserAction::Backspace)).actions,
      (Actions{HideCandidateWindow{}, ReplaceMarkedText{"きょう"}, QueryCandidates{"きょう"}}));
}

TEST(SegmentEditingTest, ACorrectedReadingLeavesAResizedSegmentAlone) {
  const InputState resized = ResizedKyou();
  const InputState corrected = resized.WithCorrectedReading("きのうは");
  EXPECT_EQ(corrected.Reading(), "きょうは");
  EXPECT_EQ(corrected.FirstSegmentReading(), "きょう");
  EXPECT_TRUE(SameCandidateList(corrected.candidates(), KyouCandidates()));
}

// Spec §3, §10: Shift+← / Shift+→ resize only while Selecting, and only with
// segment_resize; Shift-less arrows stay Backward / Forward.
TEST(SegmentEditingTest, ShiftArrowsMapToTheResizeActionsOnlyWhileSelecting) {
  const auto action = [](uint32_t key, uint32_t modifiers, K kind,
                         bool resize) -> std::optional<UserAction> {
    const auto event = MapUserAction(key, modifiers, kind, resize);
    if (!event) return std::nullopt;
    return event->action;
  };
  EXPECT_EQ(action(vk::kLeft, kModifierShift, K::Selecting, true), UserAction::ShrinkSegment);
  EXPECT_EQ(action(vk::kRight, kModifierShift, K::Selecting, true), UserAction::ExtendSegment);
  for (const bool resize : {false, true}) {
    SCOPED_TRACE(resize);
    EXPECT_EQ(action(vk::kLeft, 0, K::Selecting, resize), UserAction::Backward);
    EXPECT_EQ(action(vk::kRight, 0, K::Selecting, resize), UserAction::Forward);
    for (const K kind : {K::Idle, K::Composing, K::Previewing, K::UnicodeInput}) {
      EXPECT_FALSE(action(vk::kLeft, kModifierShift, kind, resize).has_value());
      EXPECT_FALSE(action(vk::kRight, kModifierShift, kind, resize).has_value());
    }
  }
  // Without segment_resize (the batch segments) they move like ← / →.
  EXPECT_EQ(action(vk::kLeft, kModifierShift, K::Selecting, false), UserAction::Backward);
  EXPECT_EQ(action(vk::kRight, kModifierShift, K::Selecting, false), UserAction::Forward);
  EXPECT_EQ(MapUserAction(vk::kLeft, kModifierShift, K::Selecting),
            MapUserAction(vk::kLeft, kModifierShift, K::Selecting, false));

  // The mapped Shift+← reaches the state machine as a resize.
  const auto shrink = MapUserAction(vk::kLeft, kModifierShift, K::Selecting, true);
  ASSERT_TRUE(shrink.has_value());
  EXPECT_EQ(SelectingKyouha().HandleEvent(*shrink).next.segment_end(), 3u);
}

}  // namespace
}  // namespace azookey::core
