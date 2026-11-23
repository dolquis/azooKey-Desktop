#include "azookey/core/InputState.h"

#include <optional>
#include <string_view>
#include <utility>

#include "azookey/core/CandidatePage.h"
#include "azookey/core/Utf8.h"

namespace azookey::core {
namespace {

constexpr char kUnicodePrefix[] = "U+";

bool IsAsciiLetter(char32_t cp) { return (cp >= U'a' && cp <= U'z') || (cp >= U'A' && cp <= U'Z'); }

std::optional<char> HexDigit(char32_t cp) {
  if (cp >= U'0' && cp <= U'9') return static_cast<char>(cp);
  if (cp >= U'a' && cp <= U'f') return static_cast<char>(cp - U'a' + U'A');
  if (cp >= U'A' && cp <= U'F') return static_cast<char>(cp);
  return std::nullopt;
}

// Accepts 1-8 hex digits that name a Unicode scalar value.
std::optional<char32_t> ParseUnicodeScalar(const std::string& hex) {
  if (hex.empty()) return std::nullopt;
  uint64_t value = 0;
  for (const char c : hex) {
    value = value * 16 + static_cast<uint64_t>(c <= '9' ? c - '0' : c - 'A' + 10);
  }
  if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return std::nullopt;
  return static_cast<char32_t>(value);
}

// The marks punctuationConversion converts on (spec §3.5.1, DEV-1670).
bool IsSentenceMark(char32_t cp) {
  return cp == U'、' || cp == U'。' || cp == U'？' || cp == U'！';
}

bool IsDigit(char32_t cp) { return (cp >= U'0' && cp <= U'9') || (cp >= U'０' && cp <= U'９'); }

bool IsUtf8Lead(char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }

// Code points of the reading, counted by their lead bytes as EraseLastUnit steps.
size_t CodePointCount(std::string_view text) {
  size_t count = 0;
  for (const char c : text)
    if (IsUtf8Lead(c)) ++count;
  return count;
}

// The byte offset where code point `index` starts; the size for the end.
size_t CodePointOffset(std::string_view text, size_t index) {
  size_t seen = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if (!IsUtf8Lead(text[i])) continue;
    if (seen == index) return i;
    ++seen;
  }
  return text.size();
}

}  // namespace

std::string InputState::Reading() const { return kana_ + romaji_.PreviewPending(); }

size_t InputState::segment_end() const {
  return segment_end_ != 0 ? segment_end_ : CodePointCount(kana_);
}

std::string InputState::FirstSegmentReading() const {
  if (!resizing_segment()) return kana_;
  return kana_.substr(0, CodePointOffset(kana_, segment_end_));
}

std::string InputState::RemainingReading() const {
  if (!resizing_segment()) return {};
  return kana_.substr(CodePointOffset(kana_, segment_end_));
}

InputState InputState::WithLiveConversion(bool enabled) const {
  InputState next = *this;
  next.live_conversion_ = enabled;
  return next;
}

InputState InputState::WithPunctuationConversion(bool enabled) const {
  InputState next = *this;
  next.punctuation_conversion_ = enabled;
  return next;
}

std::optional<std::string> InputState::PunctuationConversionReading(
    const UserActionEvent& event) const {
  if (!punctuation_conversion_ || event.action != UserAction::Input ||
      !IsSentenceMark(event.codepoint) ||
      (kind_ != InputStateKind::Composing && kind_ != InputStateKind::Previewing &&
       kind_ != InputStateKind::Selecting)) {
    return std::nullopt;
  }
  RomajiKanaConverter romaji = romaji_;
  std::string reading = kana_ + romaji.Flush();
  // Punctuation alone keeps its half-width candidates (DEV-1279), and a mark
  // right after a digit is a decimal point or separator, not a sentence end.
  bool punctuation_only = true;
  char32_t last = 0;
  size_t offset = 0;
  char32_t cp = 0;
  while (offset < reading.size()) {
    if (!DecodeNextUtf8(reading, offset, cp)) return std::nullopt;
    if (!IsSentenceMark(cp)) punctuation_only = false;
    last = cp;
  }
  if (punctuation_only) return std::nullopt;
  if (kind_ != InputStateKind::Selecting && IsDigit(last)) return std::nullopt;
  return reading;
}

InputState InputState::WithComposition(std::string confirmed_kana,
                                       const RomajiKanaConverter& pending_romaji) const {
  InputState next = Reset();
  next.kana_ = std::move(confirmed_kana);
  next.romaji_ = pending_romaji;
  if (!next.kana_.empty() || next.romaji_.HasPending()) next.kind_ = InputStateKind::Composing;
  return next;
}

InputState InputState::WithCorrectedReading(std::string confirmed_kana) const {
  if (confirmed_kana.empty() || resizing_segment() ||
      (kind_ != InputStateKind::Composing && kind_ != InputStateKind::Previewing &&
       kind_ != InputStateKind::Selecting)) {
    return *this;
  }
  InputState next = *this;
  next.kana_ = std::move(confirmed_kana);
  next.romaji_.Reset();
  return next;
}

InputState InputState::Reset() const {
  InputState next = *this;
  next.ResetComposition();
  return next;
}

HandleResult InputState::HandleEvent(const UserActionEvent& event,
                                     const EditContextHint& hint) const {
  // M13 transitions are document independent; hint-dependent paths (M61-A)
  // branch on the hint without changing this signature.
  static_cast<void>(hint);
  switch (event.action) {
    case UserAction::ToggleDebugWindow:
      return {{ToggleDebugWindow{}}, *this};
    case UserAction::ToggleHankaku:
      return {{ToggleInputMode{InputModeToggle::Hankaku}}, *this};
    case UserAction::ToggleHiraKata:
      return {{ToggleInputMode{InputModeToggle::HiraKata}}, *this};
    case UserAction::Forget:
      return {{ForgetLastCommit{}}, *this};
    default:
      break;
  }
  switch (kind_) {
    case InputStateKind::Idle:
      return HandleIdle(event);
    case InputStateKind::Composing:
    case InputStateKind::Previewing:
      return HandleComposing(event);
    case InputStateKind::Selecting:
      return HandleSelecting(event);
    case InputStateKind::ReplaceSuggestion:
      // Reserved (legacy-parity-spec §1.2): no transition enters it.
      return {{}, *this};
    case InputStateKind::UnicodeInput:
      return HandleUnicodeInput(event);
  }
  return {{}, *this};
}

HandleResult InputState::HandleCandidatesArrived(std::vector<Candidate> candidates) const {
  // Idle has no reading, and Selecting keeps the snapshot it displayed so a
  // late response never changes what Enter commits.
  if (kind_ != InputStateKind::Composing && kind_ != InputStateKind::Previewing) {
    return {{}, *this};
  }
  HandleResult result{{}, *this};
  InputState& next = result.next;
  next.candidates_ = std::move(candidates);
  if (next.awaiting_candidates_) {
    next.awaiting_candidates_ = false;
    if (!next.candidates_.empty()) next.OpenCandidates(result.actions);
  }
  return result;
}

HandleResult InputState::HandleSegmentCandidatesArrived(const std::string& reading,
                                                        std::vector<Candidate> candidates) const {
  // A list for an earlier end never reaches the window (spec §5.2). Without
  // candidates the frontend falls back to local ones of the reading (§5.1).
  if (kind_ != InputStateKind::Selecting || !awaiting_segment_candidates_ || candidates.empty() ||
      reading != FirstSegmentReading()) {
    return {{}, *this};
  }
  HandleResult result{{}, *this};
  InputState& next = result.next;
  next.candidates_ = std::move(candidates);
  next.selected_index_ = 0;
  next.awaiting_segment_candidates_ = false;
  result.actions.push_back(ShowCandidateWindow{next.candidates_, 0});
  result.actions.push_back(
      ReplaceMarkedText{next.candidates_.front().surface + RemainingReading()});
  return result;
}

HandleResult InputState::SelectCandidate(size_t index) const {
  if (kind_ != InputStateKind::Selecting || awaiting_segment_candidates_ ||
      index >= candidates_.size()) {
    return {{}, *this};
  }
  HandleResult result{{}, *this};
  result.next.selected_index_ = index;
  result.actions.push_back(UpdateCandidateSelection{index});
  result.actions.push_back(ReplaceMarkedText{candidates_[index].surface + RemainingReading()});
  return result;
}

HandleResult InputState::HandleIdle(const UserActionEvent& event) const {
  HandleResult result{{}, *this};
  InputState& next = result.next;
  switch (event.action) {
    case UserAction::Input:
    case UserAction::InputAlnum:
      next.AppendInput(event);
      if (next.Reading().empty()) return {{}, *this};
      next.kind_ = next.live_conversion_ ? InputStateKind::Previewing : InputStateKind::Composing;
      next.AfterReadingChanged(result.actions);
      break;
    case UserAction::UndoCommit:
      // The TIP has already written the reading into the composition
      // (commit-undo-spec §6), so only the per-key query follows. Without a
      // reading there is nothing to restore.
      if (event.reading.empty()) return {{}, *this};
      next.kana_ = event.reading;
      next.romaji_.Reset();
      next.kind_ = next.live_conversion_ ? InputStateKind::Previewing : InputStateKind::Composing;
      result.actions.push_back(QueryCandidates{next.kana_});
      break;
    case UserAction::StartUnicodeInput:
      next.kind_ = InputStateKind::UnicodeInput;
      next.unicode_hex_.clear();
      result.actions.push_back(ReplaceMarkedText{kUnicodePrefix});
      break;
    case UserAction::StartAlnumDouble:
    case UserAction::StartKanaDouble:
      // The prompt window owns its keys and its own text field, so the
      // state of this document stays Idle (legacy-parity-spec §1.2, §4.1).
      result.actions.push_back(ShowReplaceSuggestionPrompt{
          event.action == UserAction::StartAlnumDouble ? PromptPreset::English
                                                       : PromptPreset::Japanese});
      break;
    default:
      break;
  }
  return result;
}

HandleResult InputState::HandleComposing(const UserActionEvent& event) const {
  HandleResult result{{}, *this};
  InputState& next = result.next;
  switch (event.action) {
    case UserAction::Input:
    case UserAction::InputAlnum:
      // The first candidate of the reading is the one Space would show first;
      // without it the mark only joins the reading.
      if (const auto reading = PunctuationConversionReading(event);
          reading && !candidates_.empty() && candidates_.front().reading == *reading) {
        next.CommitCandidate(0, result.actions, event.codepoint);
        break;
      }
      next.AppendInput(event);
      if (next.live_conversion_) next.kind_ = InputStateKind::Previewing;
      next.AfterReadingChanged(result.actions);
      break;
    case UserAction::Backspace:
      next.EraseLastUnit();
      next.AfterReadingChanged(result.actions);
      break;
    case UserAction::StartConversion:
    case UserAction::NextCandidate:
    case UserAction::PrevCandidate:
      next.StartConversion(result.actions);
      break;
    case UserAction::Commit:
      next.CommitAsIs(result.actions);
      break;
    case UserAction::Cancel:
      next.ResetComposition();
      result.actions.push_back(CancelMarkedText{});
      break;
    case UserAction::StartUnicodeInput:
      next.CommitAsIs(result.actions);
      next.kind_ = InputStateKind::UnicodeInput;
      result.actions.push_back(ReplaceMarkedText{kUnicodePrefix});
      break;
    default:
      // No cursor inside the composition yet: editing and navigation keys
      // leave the reading untouched.
      break;
  }
  return result;
}

HandleResult InputState::HandleSelecting(const UserActionEvent& event) const {
  HandleResult result{{}, *this};
  InputState& next = result.next;
  // segment-editing-spec §4, also while the first segment's candidates are
  // awaited (§5.2): one code point at a time.
  if (event.action == UserAction::ShrinkSegment) {
    if (segment_end() >= 2) next.ResizeSegment(segment_end() - 1, result.actions);
    return result;
  }
  if (event.action == UserAction::ExtendSegment) {
    // Before any resize the segment starts over from one code point; after one
    // it stops at the whole reading, as in legacy.
    if (resizing_segment()) {
      next.ResizeSegment(segment_end_ + 1, result.actions);
    } else if (!segment_resized_ && segment_end() >= 2) {
      next.ResizeSegment(1, result.actions);
    }
    return result;
  }
  // Spec §5.2: the frontend holds the other keys until the first segment's
  // candidates arrive and replays them then; Esc cancels at once.
  if (awaiting_segment_candidates_ && event.action != UserAction::Cancel) return result;
  const size_t count = candidates_.size();
  // While resized the remaining reading follows the first segment as typed.
  const std::string remaining = RemainingReading();
  auto select = [&](size_t index) {
    next.selected_index_ = index;
    result.actions.push_back(UpdateCandidateSelection{index});
    result.actions.push_back(ReplaceMarkedText{candidates_[index].surface + remaining});
  };
  // Enter, →, Ctrl+F and the digits commit only the first segment while it is
  // resized (spec §4, §6.1).
  auto commit = [&](size_t index) {
    if (resizing_segment()) {
      next.PartialCommit(index, result.actions);
    } else {
      next.CommitCandidate(index, result.actions);
    }
  };
  auto move_selection = [&](bool forward) {
    select(forward ? (selected_index_ + 1) % count : (selected_index_ + count - 1) % count);
  };
  // PageUp / PageDown: the same position on the previous / next page; nothing
  // with one page (spec §1.4「候補のページ」).
  if (event.by_page && (event.action == UserAction::Up || event.action == UserAction::Down)) {
    if (const auto target = CandidateOnAdjacentPage(
            selected_index_, event.action == UserAction::Down, count, event.page_starts))
      select(*target);
    return result;
  }
  switch (event.action) {
    case UserAction::Input:
    case UserAction::InputAlnum: {
      if (PunctuationConversionReading(event)) {
        next.CommitCandidate(selected_index_, result.actions, event.codepoint);
        break;
      }
      // Typing commits the highlight like Enter, then the key starts a new
      // reading as if typed from Idle (DEV-1735, legacy parity). While resized
      // it commits what is shown (segment-editing-spec §4).
      next.CommitCandidate(selected_index_, result.actions);
      HandleResult resumed = next.HandleIdle(event);
      for (auto& action : resumed.actions) result.actions.push_back(std::move(action));
      result.next = std::move(resumed.next);
      break;
    }
    case UserAction::Backspace:
      // Backspace is the one key that reopens the reading without committing.
      // A resized first segment is dropped for the whole reading first.
      result.actions.push_back(HideCandidateWindow{});
      next.kind_ = live_conversion_ ? InputStateKind::Previewing : InputStateKind::Composing;
      next.selected_index_ = 0;
      next.ClearSegment();
      next.EraseLastUnit();
      next.AfterReadingChanged(result.actions);
      break;
    case UserAction::StartConversion:
    case UserAction::NextCandidate:
    case UserAction::Down:
      move_selection(true);
      break;
    case UserAction::PrevCandidate:
    case UserAction::Up:
      move_selection(false);
      break;
    case UserAction::SelectByDigit:
      // The n-th candidate of the current page; nothing when it has none.
      if (const auto target =
              CandidateByDigit(selected_index_, event.digit, count, event.page_starts))
        commit(*target);
      break;
    case UserAction::Commit:
      commit(selected_index_);
      break;
    case UserAction::Forward:
      // With the whole reading → goes to the app (legacy-parity-spec §1.4).
      if (resizing_segment()) next.PartialCommit(selected_index_, result.actions);
      break;
    case UserAction::CommitHiragana:
    case UserAction::CommitKatakana:
    case UserAction::CommitHalfwidthKatakana:
    case UserAction::CommitFullwidthRoman:
    case UserAction::CommitHalfwidthRoman:
      // Spec §6.1: the frontend commits the first segment's reading in the
      // form (tsf-deep-integration-spec §3.6); the remaining reading's query
      // follows. With the whole reading the frontend does it all.
      if (resizing_segment()) {
        result.actions.push_back(HideCandidateWindow{});
        next.ContinueWithRemainingReading(result.actions);
      }
      break;
    case UserAction::Cancel:
      // One Esc cancels the whole input (DEV-1649). Nothing is committed, so no
      // ObserveCommit is emitted; the candidate cache goes with the reading.
      next.ResetComposition();
      result.actions.push_back(HideCandidateWindow{});
      result.actions.push_back(CancelMarkedText{});
      break;
    case UserAction::StartUnicodeInput:
      next.CommitCandidate(selected_index_, result.actions);
      next.kind_ = InputStateKind::UnicodeInput;
      result.actions.push_back(ReplaceMarkedText{kUnicodePrefix});
      break;
    default:
      break;
  }
  return result;
}

HandleResult InputState::HandleUnicodeInput(const UserActionEvent& event) const {
  HandleResult result{{}, *this};
  InputState& next = result.next;
  // Ends the mode. Returns false when the buffer does not name a scalar.
  auto finish = [&](bool beep_on_invalid) {
    if (unicode_hex_.empty()) {
      result.actions.push_back(CancelMarkedText{});
      next.ResetComposition();
      return true;
    }
    const auto scalar = ParseUnicodeScalar(unicode_hex_);
    if (!scalar) {
      if (beep_on_invalid) result.actions.push_back(PlayBeep{});
      return false;
    }
    std::string text;
    AppendUtf8(text, *scalar);
    result.actions.push_back(ReplaceMarkedText{std::move(text)});
    result.actions.push_back(CommitMarkedText{});
    next.ResetComposition();
    return true;
  };
  switch (event.action) {
    case UserAction::Input:
    case UserAction::InputAlnum:
      if (const auto digit = HexDigit(event.codepoint)) {
        if (unicode_hex_.size() >= kMaxUnicodeHexDigits) {
          result.actions.push_back(PlayBeep{});
          break;
        }
        next.unicode_hex_.push_back(*digit);
        result.actions.push_back(ReplaceMarkedText{kUnicodePrefix + next.unicode_hex_});
        break;
      }
      // A non-hex key confirms the buffer and continues as normal input.
      if (!finish(false)) {
        result.actions.push_back(PlayBeep{});
        result.actions.push_back(CancelMarkedText{});
        next.ResetComposition();
      }
      {
        HandleResult resumed = next.HandleIdle(event);
        for (auto& action : resumed.actions) result.actions.push_back(std::move(action));
        result.next = std::move(resumed.next);
      }
      break;
    case UserAction::Backspace:
      if (unicode_hex_.empty()) {
        next.ResetComposition();
        result.actions.push_back(CancelMarkedText{});
        break;
      }
      next.unicode_hex_.pop_back();
      result.actions.push_back(ReplaceMarkedText{kUnicodePrefix + next.unicode_hex_});
      break;
    case UserAction::Commit:
      finish(true);
      break;
    case UserAction::Cancel:
      next.ResetComposition();
      result.actions.push_back(CancelMarkedText{});
      break;
    default:
      break;
  }
  return result;
}

void InputState::AppendInput(const UserActionEvent& event) {
  const char32_t cp = event.codepoint;
  if (cp == 0) return;
  // Keep ordinary punctuation literal, but allow a custom rule to consume
  // printable ASCII symbols or digits while its prefix is still pending.
  if (event.action == UserAction::Input && cp <= 0x7f &&
      (IsAsciiLetter(cp) || cp == U'-' || (cp == U'\'' && romaji_.SeparatesNWithApostrophe()) ||
       romaji_.CanContinueCustomWith(static_cast<char>(cp)))) {
    kana_ += romaji_.Feed(static_cast<char>(cp));
    return;
  }
  kana_ += romaji_.Flush();
  AppendUtf8(kana_, cp);
}

void InputState::EraseLastUnit() {
  // One unit is a pending romaji preview step, otherwise one kana codepoint.
  if (romaji_.HasPending()) {
    romaji_.PopPendingPreview();
    return;
  }
  size_t i = kana_.size();
  while (i > 0 && (static_cast<unsigned char>(kana_[i - 1]) & 0xC0) == 0x80) --i;
  if (i > 0) --i;
  kana_.erase(i);
}

void InputState::ResetComposition() {
  kind_ = InputStateKind::Idle;
  kana_.clear();
  romaji_.Reset();
  candidates_.clear();
  selected_index_ = 0;
  awaiting_candidates_ = false;
  ClearSegment();
  unicode_hex_.clear();
}

void InputState::ClearSegment() {
  segment_end_ = 0;
  segment_resized_ = false;
  whole_candidates_.clear();
  awaiting_segment_candidates_ = false;
}

void InputState::AfterReadingChanged(std::vector<ClientAction>& actions) {
  candidates_.clear();
  awaiting_candidates_ = false;
  const std::string reading = Reading();
  if (reading.empty()) {
    ResetComposition();
    actions.push_back(CancelMarkedText{});
    return;
  }
  actions.push_back(ReplaceMarkedText{reading});
  actions.push_back(QueryCandidates{reading});
}

void InputState::StartConversion(std::vector<ClientAction>& actions) {
  const std::string flushed = romaji_.Flush();
  if (!flushed.empty()) {
    kana_ += flushed;
    actions.push_back(ReplaceMarkedText{kana_});
  }
  if (kana_.empty()) return;
  // The per-key results cached while typing are the live answer (one model
  // candidate, no typo suggestion or English), so the window always opens from
  // a fresh full query (DEV-1653). Until HandleCandidatesArrived the current
  // kind stays, so Enter still commits as-is and digits pass through (§1.5.4).
  candidates_.clear();
  actions.push_back(QueryCandidates{kana_, true});
  awaiting_candidates_ = true;
}

void InputState::OpenCandidates(std::vector<ClientAction>& actions) {
  kind_ = InputStateKind::Selecting;
  selected_index_ = 0;
  awaiting_candidates_ = false;
  // Every Selecting starts with the whole reading as the first segment
  // (segment-editing-spec §3), also the one a partial commit's remaining
  // reading opens (§6.1).
  ClearSegment();
  whole_candidates_ = candidates_;
  actions.push_back(ShowCandidateWindow{candidates_, 0});
  actions.push_back(ReplaceMarkedText{candidates_.front().surface});
}

void InputState::ResizeSegment(size_t end, std::vector<ClientAction>& actions) {
  segment_resized_ = true;
  if (end >= CodePointCount(kana_)) {
    // Back to the whole reading: the list Selecting was entered with and its
    // first candidate, without a query (spec §4). An awaited answer is dropped.
    segment_end_ = 0;
    awaiting_segment_candidates_ = false;
    candidates_ = whole_candidates_;
    selected_index_ = 0;
    actions.push_back(ShowCandidateWindow{candidates_, 0});
    actions.push_back(ReplaceMarkedText{candidates_.front().surface});
    return;
  }
  // Until the answer the first segment shows as its reading and the window
  // keeps the previous list (spec §5.2, §7).
  segment_end_ = end;
  awaiting_segment_candidates_ = true;
  actions.push_back(ReplaceMarkedText{kana_});
  actions.push_back(QueryCandidates{FirstSegmentReading(), true, RemainingReading()});
}

void InputState::PartialCommit(size_t index, std::vector<ClientAction>& actions) {
  const std::string surface = candidates_[index].surface;
  actions.push_back(HideCandidateWindow{});
  actions.push_back(CommitFirstSegment{surface, RemainingReading()});
  actions.push_back(ObserveCommit{FirstSegmentReading(), surface});
  ContinueWithRemainingReading(actions);
}

void InputState::ContinueWithRemainingReading(std::vector<ClientAction>& actions) {
  std::string remaining = RemainingReading();
  ResetComposition();
  kana_ = std::move(remaining);
  // Waits like StartConversion (legacy-parity-spec §1.2): the answer opens a
  // Selecting whose first segment is the whole remaining reading (spec §6.1).
  kind_ = live_conversion_ ? InputStateKind::Previewing : InputStateKind::Composing;
  awaiting_candidates_ = true;
  actions.push_back(QueryCandidates{kana_, true});
}

void InputState::CommitCandidate(size_t index, std::vector<ClientAction>& actions,
                                 char32_t punctuation) {
  // While resized the remaining reading is committed as typed, and only the
  // first segment is observed (segment-editing-spec §4, §8).
  const std::string reading = resizing_segment() ? FirstSegmentReading() : kana_ + romaji_.Flush();
  const std::string surface = candidates_[index].surface;
  std::string text = surface + RemainingReading();
  if (punctuation != 0) AppendUtf8(text, punctuation);
  actions.push_back(HideCandidateWindow{});
  actions.push_back(ReplaceMarkedText{std::move(text)});
  actions.push_back(CommitMarkedText{});
  actions.push_back(ObserveCommit{reading, surface});
  ResetComposition();
}

void InputState::CommitAsIs(std::vector<ClientAction>& actions) {
  kana_ += romaji_.Flush();
  if (!kana_.empty()) {
    actions.push_back(ReplaceMarkedText{kana_});
    actions.push_back(CommitMarkedText{});
  }
  ResetComposition();
}

}  // namespace azookey::core
