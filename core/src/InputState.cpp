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

bool IsCharacterInput(UserAction action) {
  return action == UserAction::Input || action == UserAction::InputAlnum;
}

}  // namespace

std::string InputState::Reading() const { return kana_ + romaji_.PreviewPending(); }

InputState InputState::WithLiveConversion(bool enabled) const {
  InputState next = *this;
  next.live_conversion_ = enabled;
  return next;
}

InputState InputState::WithComposition(std::string confirmed_kana,
                                       const RomajiKanaConverter& pending_romaji) const {
  InputState next = Reset();
  next.kana_ = std::move(confirmed_kana);
  next.romaji_ = pending_romaji;
  if (!next.kana_.empty() || next.romaji_.HasPending()) next.kind_ = InputStateKind::Composing;
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
      next.kind_ = InputStateKind::Composing;
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
    case UserAction::InputAlnum:
    case UserAction::Backspace:
      // Resuming edits closes the window without committing the highlight.
      result.actions.push_back(HideCandidateWindow{});
      next.kind_ = InputStateKind::Composing;
      next.selected_index_ = 0;
      if (IsCharacterInput(event.action)) {
        next.AppendInput(event);
      } else {
        next.EraseLastUnit();
      }
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
      // Esc returns to the reading and keeps the cached candidates (M10).
      result.actions.push_back(HideCandidateWindow{});
      next.kind_ = InputStateKind::Composing;
      next.selected_index_ = 0;
      result.actions.push_back(ReplaceMarkedText{next.Reading()});
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
  // Letters and the long-vowel hyphen go through romaji; anything else is
  // literal and first settles the pending romaji, like explicit punctuation.
  if (event.action == UserAction::Input && (IsAsciiLetter(cp) || cp == U'-')) {
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
    candidates_.clear();
    actions.push_back(ReplaceMarkedText{kana_});
  }
  if (kana_.empty()) return;
  if (!candidates_.empty()) {
    OpenCandidates(actions);
    return;
  }
  // Cache miss: stay in the current kind until HandleCandidatesArrived so
  // Enter still commits as-is and digits pass through (§1.5.4).
  actions.push_back(QueryCandidates{kana_});
  awaiting_candidates_ = true;
}

void InputState::OpenCandidates(std::vector<ClientAction>& actions) {
  kind_ = InputStateKind::Selecting;
  selected_index_ = 0;
  awaiting_candidates_ = false;
  actions.push_back(ShowCandidateWindow{candidates_, 0});
  actions.push_back(ReplaceMarkedText{candidates_.front().surface});
}

void InputState::CommitCandidate(size_t index, std::vector<ClientAction>& actions) {
  const std::string reading = kana_ + romaji_.Flush();
  const std::string surface = candidates_[index].surface;
  actions.push_back(HideCandidateWindow{});
  actions.push_back(ReplaceMarkedText{surface});
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
