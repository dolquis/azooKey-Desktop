#include "azookey/core/InputState.h"

#include <optional>
#include <utility>

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

}  // namespace

std::string InputState::Reading() const { return kana_ + romaji_.PreviewPending(); }

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
  if (confirmed_kana.empty() ||
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
      return HandleReplaceSuggestion(event);
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
    case UserAction::StartUnicodeInput:
      next.kind_ = InputStateKind::UnicodeInput;
      next.unicode_hex_.clear();
      result.actions.push_back(ReplaceMarkedText{kUnicodePrefix});
      break;
    case UserAction::StartAlnumDouble:
    case UserAction::StartKanaDouble:
      next.kind_ = InputStateKind::ReplaceSuggestion;
      next.replace_suggestion_mode_ = event.action == UserAction::StartAlnumDouble
                                          ? ReplaceSuggestionMode::MagicConversion
                                          : ReplaceSuggestionMode::ReplaceSuggestion;
      result.actions.push_back(ShowReplaceSuggestionPrompt{next.replace_suggestion_mode_});
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
  const size_t count = candidates_.size();
  auto move_selection = [&](bool forward) {
    next.selected_index_ =
        forward ? (selected_index_ + 1) % count : (selected_index_ + count - 1) % count;
    result.actions.push_back(UpdateCandidateSelection{next.selected_index_});
    result.actions.push_back(ReplaceMarkedText{candidates_[next.selected_index_].surface});
  };
  switch (event.action) {
    case UserAction::Input:
    case UserAction::InputAlnum: {
      if (PunctuationConversionReading(event)) {
        next.CommitCandidate(selected_index_, result.actions, event.codepoint);
        break;
      }
      // Typing commits the highlight like Enter, then the key starts a new
      // reading as if typed from Idle (DEV-1735, legacy parity).
      next.CommitCandidate(selected_index_, result.actions);
      HandleResult resumed = next.HandleIdle(event);
      for (auto& action : resumed.actions) result.actions.push_back(std::move(action));
      result.next = std::move(resumed.next);
      break;
    }
    case UserAction::Backspace:
      // Backspace is the one key that reopens the reading without committing.
      result.actions.push_back(HideCandidateWindow{});
      next.kind_ = live_conversion_ ? InputStateKind::Previewing : InputStateKind::Composing;
      next.selected_index_ = 0;
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
      if (event.digit >= 1 && static_cast<size_t>(event.digit) <= count) {
        next.CommitCandidate(static_cast<size_t>(event.digit - 1), result.actions);
      }
      break;
    case UserAction::Commit:
      next.CommitCandidate(selected_index_, result.actions);
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

HandleResult InputState::HandleReplaceSuggestion(const UserActionEvent& event) const {
  HandleResult result{{}, *this};
  InputState& next = result.next;
  switch (event.action) {
    case UserAction::Commit:
      next.ResetComposition();
      result.actions.push_back(SubmitReplaceSuggestion{});
      break;
    case UserAction::Cancel:
      next.ResetComposition();
      result.actions.push_back(HideReplaceSuggestionPrompt{});
      break;
    default:
      // The prompt UI owns text entry while it is open.
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
  unicode_hex_.clear();
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
  actions.push_back(ShowCandidateWindow{candidates_, 0});
  actions.push_back(ReplaceMarkedText{candidates_.front().surface});
}

void InputState::CommitCandidate(size_t index, std::vector<ClientAction>& actions,
                                 char32_t punctuation) {
  const std::string reading = kana_ + romaji_.Flush();
  const std::string surface = candidates_[index].surface;
  std::string text = surface;
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
