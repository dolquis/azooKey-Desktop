#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "azookey/core/Candidate.h"
#include "azookey/core/ClientAction.h"
#include "azookey/core/EditContextHint.h"
#include "azookey/core/RomajiKanaConverter.h"
#include "azookey/core/UserAction.h"

namespace azookey::core {

// Append-only (docs/legacy-parity-spec.md §1.5.2). Never reorder or renumber.
enum class InputStateKind : uint8_t {
  Idle,
  Composing,
  Previewing,
  Selecting,
  ReplaceSuggestion,
  UnicodeInput,
};

struct HandleResult;

// Pure, TSF-independent input state machine (docs/legacy-parity-spec.md §1.2,
// §1.5.1). The same (state, event, hint) always yields the same actions and
// next state. Document facts reach the core only through EditContextHint;
// request staleness and windows stay in the frontend.
class InputState {
 public:
  static constexpr size_t kMaxUnicodeHexDigits = 8;

  InputStateKind kind() const { return kind_; }
  // Confirmed kana plus the preview of pending romaji.
  std::string Reading() const;
  const std::string& confirmed_kana() const { return kana_; }
  const RomajiKanaConverter& pending_romaji() const { return romaji_; }
  // Latest candidates delivered for the current reading, or the fixed
  // snapshot while Selecting.
  const std::vector<Candidate>& candidates() const { return candidates_; }
  size_t selected_index() const { return selected_index_; }
  // StartConversion missed the candidate cache and waits for a response.
  bool awaiting_candidates() const { return awaiting_candidates_; }
  const std::string& unicode_hex() const { return unicode_hex_; }
  bool live_conversion() const { return live_conversion_; }
  ReplaceSuggestionMode replace_suggestion_mode() const { return replace_suggestion_mode_; }

  // Setting injected by the frontend. Only affects later Input events.
  InputState WithLiveConversion(bool enabled) const;
  // Import composition state when returning from a frontend-owned input path.
  // The frontend owns the corresponding marked-text update; no actions are emitted.
  InputState WithComposition(std::string confirmed_kana,
                             const RomajiKanaConverter& pending_romaji = {}) const;
  // Discard logical composition state without emitting TSF actions.
  InputState Reset() const;

  HandleResult HandleEvent(const UserActionEvent& event, const EditContextHint& hint = {}) const;
  // Feedback event: the frontend delivers a fresh (non-stale) candidate
  // response for the current reading.
  HandleResult HandleCandidatesArrived(std::vector<Candidate> candidates) const;

 private:
  HandleResult HandleIdle(const UserActionEvent& event) const;
  HandleResult HandleComposing(const UserActionEvent& event) const;
  HandleResult HandleSelecting(const UserActionEvent& event) const;
  HandleResult HandleReplaceSuggestion(const UserActionEvent& event) const;
  HandleResult HandleUnicodeInput(const UserActionEvent& event) const;

  void AppendInput(const UserActionEvent& event);
  void EraseLastUnit();
  void ResetComposition();
  void StartConversion(std::vector<ClientAction>& actions);
  void OpenCandidates(std::vector<ClientAction>& actions);
  void CommitCandidate(size_t index, std::vector<ClientAction>& actions);
  void CommitAsIs(std::vector<ClientAction>& actions);
  void AfterReadingChanged(std::vector<ClientAction>& actions);

  InputStateKind kind_{InputStateKind::Idle};
  std::string kana_;
  RomajiKanaConverter romaji_;
  std::vector<Candidate> candidates_;
  size_t selected_index_{0};
  bool awaiting_candidates_{false};
  std::string unicode_hex_;
  bool live_conversion_{false};
  ReplaceSuggestionMode replace_suggestion_mode_{ReplaceSuggestionMode::MagicConversion};
};

struct HandleResult {
  // Applied in order by the frontend.
  std::vector<ClientAction> actions;
  InputState next;
};

}  // namespace azookey::core
