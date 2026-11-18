#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace azookey::core {

// Semantic key intent. Append-only (docs/legacy-parity-spec.md §1.5.2): new
// characters reuse Input / InputAlnum with a codepoint instead of new values.
enum class UserAction : uint16_t {
  // Character input.
  Input,
  InputAlnum,

  // Editing.
  Backspace,
  Delete,
  Forward,
  Backward,
  Up,
  Down,
  LineHead,
  LineEnd,

  // Conversion control.
  StartConversion,
  NextCandidate,
  PrevCandidate,
  SelectByDigit,
  Commit,
  Cancel,

  // Mode switching.
  ToggleHankaku,
  ToggleHiraKata,
  StartUnicodeInput,
  StartAlnumDouble,
  StartKanaDouble,

  // Learning.
  Forget,

  // Debugging.
  ToggleDebugWindow,
  ToggleAlnum,

  // F6-F10 commit the reading in one character form
  // (docs/tsf-deep-integration-spec.md §3.6). The TIP commits them on every
  // input path with BuildCommitForm; InputState leaves them unhandled, except
  // that a resized first segment asks for the remaining reading's candidates
  // (docs/segment-editing-spec.md §6.1).
  CommitHiragana,
  CommitKatakana,
  CommitHalfwidthKatakana,
  CommitFullwidthRoman,
  CommitHalfwidthRoman,

  // Ctrl+Backspace in Idle right after a commit the TIP can undo
  // (docs/commit-undo-spec.md §3). The TIP checks §4 and edits the document
  // (§6) first, then passes the reading it restored in UserActionEvent::reading.
  UndoCommit,

  // Shift+← / Shift+→ while Selecting a normal conversion shrink and extend
  // the first segment (docs/segment-editing-spec.md §4).
  ShrinkSegment,
  ExtendSegment,
};

inline constexpr uint32_t kModifierShift = 1u << 0;
inline constexpr uint32_t kModifierCtrl = 1u << 1;
inline constexpr uint32_t kModifierAlt = 1u << 2;
inline constexpr uint32_t kModifierWin = 1u << 3;

struct UserActionEvent {
  UserAction action{UserAction::Input};
  // Resolved by the TIP for Input / InputAlnum only.
  char32_t codepoint{0};
  uint32_t modifiers{0};
  // 1-9 for SelectByDigit: the n-th candidate of the current page.
  int digit{0};
  // Up / Down from PageUp / PageDown move a page instead of one candidate
  // (docs/legacy-parity-spec.md §1.4「候補のページ」, §1.6.2 (B)).
  bool by_page{false};
  // The page split an app set with SetPageIndex, which the TIP attaches to
  // SelectByDigit and paged Up / Down; empty for nine candidates per page
  // (core/include/azookey/core/CandidatePage.h).
  std::vector<uint32_t> page_starts;
  // The reading UndoCommit restores as the composition's reading
  // (docs/commit-undo-spec.md §5); the TIP sets it on UndoCommit only. It is
  // kana: the reading the commit held, with no pending romaji.
  std::string reading;

  friend bool operator==(const UserActionEvent&, const UserActionEvent&) = default;
};

}  // namespace azookey::core
