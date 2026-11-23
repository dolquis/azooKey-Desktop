#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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
  // Reserved for legacy's Ctrl+S suggestion; no transition enters it
  // (docs/legacy-parity-spec.md §1.2).
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
  // snapshot while Selecting; while the first segment is resized, the list
  // for it (docs/segment-editing-spec.md §3).
  const std::vector<Candidate>& candidates() const { return candidates_; }
  size_t selected_index() const { return selected_index_; }
  // StartConversion missed the candidate cache and waits for a response.
  bool awaiting_candidates() const { return awaiting_candidates_; }
  const std::string& unicode_hex() const { return unicode_hex_; }
  // The first segment while Selecting (docs/segment-editing-spec.md §2, §3).
  // segment_end() is its end e in code points, which is the reading's length n
  // unless the segment has been shrunk; the remaining reading is empty then.
  size_t segment_end() const;
  bool resizing_segment() const { return segment_end_ != 0; }
  std::string FirstSegmentReading() const;
  std::string RemainingReading() const;
  // A resize waits for the first segment's candidates (spec §5.2).
  bool awaiting_segment_candidates() const { return awaiting_segment_candidates_; }
  bool live_conversion() const { return live_conversion_; }
  bool punctuation_conversion() const { return punctuation_conversion_; }

  // Setting injected by the frontend. Only affects later Input events.
  InputState WithLiveConversion(bool enabled) const;
  // punctuationConversion = "commit" (docs/tsf-deep-integration-spec.md
  // §3.5.1). Only affects later Input events.
  InputState WithPunctuationConversion(bool enabled) const;
  // The reading a 、 。 ？ or ！ key converts and commits, before the mark is
  // added; nullopt when the key only joins the reading. In Composing and
  // Previewing the commit also needs a cached first candidate of this reading.
  std::optional<std::string> PunctuationConversionReading(const UserActionEvent& event) const;
  // Import composition state when returning from a frontend-owned input path.
  // The frontend owns the corresponding marked-text update; no actions are emitted.
  InputState WithComposition(std::string confirmed_kana,
                             const RomajiKanaConverter& pending_romaji = {}) const;
  // Apply an accepted corrected reading while preserving active state and candidates.
  // Clears pending romaji; empty readings, inactive states and a resized first
  // segment (whose answers are never corrected, segment-editing-spec §5.1)
  // leave the state unchanged.
  InputState WithCorrectedReading(std::string confirmed_kana) const;
  // Discard logical composition state without emitting TSF actions.
  InputState Reset() const;

  HandleResult HandleEvent(const UserActionEvent& event, const EditContextHint& hint = {}) const;
  // Feedback event: the frontend delivers a fresh (non-stale) candidate
  // response for the current reading.
  HandleResult HandleCandidatesArrived(std::vector<Candidate> candidates) const;
  // Feedback event: the candidates of `reading`, the first segment a resize
  // asked for (segment-editing-spec §5.2). They replace the list only while
  // the state waits for an answer for that reading; an answer for another
  // reading and an empty one change nothing. Telling a stale answer for the
  // same reading apart stays with the frontend's request ids (§5.1).
  HandleResult HandleSegmentCandidatesArrived(const std::string& reading,
                                              std::vector<Candidate> candidates) const;
  // An app that draws the candidate list selects candidate `index` with
  // ITfCandidateListUIElementBehavior::SetSelection (tsf-deep-integration-spec
  // §2.3): the same move as the arrow keys, onto any candidate of the list
  // (the first segment's while resized, segment-editing-spec §4). Nothing
  // outside Selecting, while the first segment's candidates are awaited
  // (§5.2), or for an index past the list.
  HandleResult SelectCandidate(size_t index) const;

 private:
  HandleResult HandleIdle(const UserActionEvent& event) const;
  HandleResult HandleComposing(const UserActionEvent& event) const;
  HandleResult HandleSelecting(const UserActionEvent& event) const;
  HandleResult HandleUnicodeInput(const UserActionEvent& event) const;

  void AppendInput(const UserActionEvent& event);
  void EraseLastUnit();
  void ResetComposition();
  void StartConversion(std::vector<ClientAction>& actions);
  void OpenCandidates(std::vector<ClientAction>& actions);
  // A nonzero punctuation is committed after the candidate but never observed.
  void CommitCandidate(size_t index, std::vector<ClientAction>& actions, char32_t punctuation = 0);
  void CommitAsIs(std::vector<ClientAction>& actions);
  void AfterReadingChanged(std::vector<ClientAction>& actions);
  // segment-editing-spec §4, §5: moves e to `end`; an end of n returns to the
  // whole reading's candidates.
  void ResizeSegment(size_t end, std::vector<ClientAction>& actions);
  // Spec §6.1: commits the first segment with candidate `index` and waits for
  // the remaining reading's candidates.
  void PartialCommit(size_t index, std::vector<ClientAction>& actions);
  // The remaining reading becomes the reading and its window query goes out.
  void ContinueWithRemainingReading(std::vector<ClientAction>& actions);
  void ClearSegment();

  InputStateKind kind_{InputStateKind::Idle};
  std::string kana_;
  RomajiKanaConverter romaji_;
  std::vector<Candidate> candidates_;
  size_t selected_index_{0};
  bool awaiting_candidates_{false};
  // Selecting (segment-editing-spec §3): 0 while the first segment is the
  // whole reading, otherwise its end e in code points (0 < e < n).
  size_t segment_end_{0};
  // Whether this Selecting has resized the first segment.
  bool segment_resized_{false};
  // The whole reading's candidates as Selecting was entered.
  std::vector<Candidate> whole_candidates_;
  bool awaiting_segment_candidates_{false};
  std::string unicode_hex_;
  bool live_conversion_{false};
  bool punctuation_conversion_{false};
};

struct HandleResult {
  // Applied in order by the frontend.
  std::vector<ClientAction> actions;
  InputState next;
};

}  // namespace azookey::core
