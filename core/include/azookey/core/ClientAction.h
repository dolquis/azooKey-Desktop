#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "azookey/core/Candidate.h"

namespace azookey::core {

// Side-effect instructions emitted by InputState and applied in order by the
// frontend. Append-only fine-grained actions (docs/legacy-parity-spec.md §1.3,
// §1.5.2): combined effects are sequences, never composite alternatives.

// Replace the whole marked text. An empty text leaves an empty composition.
struct ReplaceMarkedText {
  std::string text;
  friend bool operator==(const ReplaceMarkedText&, const ReplaceMarkedText&) = default;
};
struct AppendToMarkedText {
  std::string text;
  friend bool operator==(const AppendToMarkedText&, const AppendToMarkedText&) = default;
};
// Commit the marked text as currently displayed and end the composition.
struct CommitMarkedText {
  friend bool operator==(const CommitMarkedText&, const CommitMarkedText&) = default;
};
// Clear the marked text and end the composition without committing.
struct CancelMarkedText {
  friend bool operator==(const CancelMarkedText&, const CancelMarkedText&) = default;
};
// Candidates compare by what the user sees and commits.
inline bool SameCandidateList(const std::vector<Candidate>& lhs,
                              const std::vector<Candidate>& rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i].surface != rhs[i].surface || lhs[i].reading != rhs[i].reading) return false;
  }
  return true;
}

struct ShowCandidateWindow {
  std::vector<Candidate> candidates;
  size_t selected_index{0};
  friend bool operator==(const ShowCandidateWindow& lhs, const ShowCandidateWindow& rhs) {
    return lhs.selected_index == rhs.selected_index &&
           SameCandidateList(lhs.candidates, rhs.candidates);
  }
};
struct HideCandidateWindow {
  friend bool operator==(const HideCandidateWindow&, const HideCandidateWindow&) = default;
};
struct ShowPredictionWindow {
  std::vector<Candidate> candidates;
  friend bool operator==(const ShowPredictionWindow& lhs, const ShowPredictionWindow& rhs) {
    return SameCandidateList(lhs.candidates, rhs.candidates);
  }
};
struct HidePredictionWindow {
  friend bool operator==(const HidePredictionWindow&, const HidePredictionWindow&) = default;
};
struct ReplaceSelectedText {
  std::string text;
  friend bool operator==(const ReplaceSelectedText&, const ReplaceSelectedText&) = default;
};
struct PlayBeep {
  friend bool operator==(const PlayBeep&, const PlayBeep&) = default;
};
// Ask the backend for candidates of the reading. Staleness stays in the TIP.
struct QueryCandidates {
  std::string reading;
  friend bool operator==(const QueryCandidates&, const QueryCandidates&) = default;
};
// Move only the highlight of the visible candidate window.
struct UpdateCandidateSelection {
  size_t index{0};
  friend bool operator==(const UpdateCandidateSelection&,
                         const UpdateCandidateSelection&) = default;
};
// Record a committed candidate for learning.
struct ObserveCommit {
  std::string reading;
  std::string surface;
  friend bool operator==(const ObserveCommit&, const ObserveCommit&) = default;
};

enum class InputModeToggle : uint8_t {
  Hankaku,
  HiraKata,
};
struct ToggleInputMode {
  InputModeToggle toggle{InputModeToggle::Hankaku};
  friend bool operator==(const ToggleInputMode&, const ToggleInputMode&) = default;
};

enum class ReplaceSuggestionMode : uint8_t {
  // Alnum double tap: free-form AI conversion (Magic Conversion).
  MagicConversion,
  // Kana double tap: rewrite the selected text (Replace Suggestion).
  ReplaceSuggestion,
};
// Read the selection and show the prompt UI.
struct ShowReplaceSuggestionPrompt {
  ReplaceSuggestionMode mode{ReplaceSuggestionMode::MagicConversion};
  friend bool operator==(const ShowReplaceSuggestionPrompt&,
                         const ShowReplaceSuggestionPrompt&) = default;
};
// Send the prompt; the response is applied with ReplaceSelectedText.
struct SubmitReplaceSuggestion {
  friend bool operator==(const SubmitReplaceSuggestion&, const SubmitReplaceSuggestion&) = default;
};
struct HideReplaceSuggestionPrompt {
  friend bool operator==(const HideReplaceSuggestionPrompt&,
                         const HideReplaceSuggestionPrompt&) = default;
};
struct ForgetLastCommit {
  friend bool operator==(const ForgetLastCommit&, const ForgetLastCommit&) = default;
};
struct ToggleDebugWindow {
  friend bool operator==(const ToggleDebugWindow&, const ToggleDebugWindow&) = default;
};

using ClientAction =
    std::variant<AppendToMarkedText, ReplaceMarkedText, CommitMarkedText, CancelMarkedText,
                 ShowCandidateWindow, HideCandidateWindow, ShowPredictionWindow,
                 HidePredictionWindow, ReplaceSelectedText, PlayBeep, QueryCandidates,
                 UpdateCandidateSelection, ObserveCommit, ToggleInputMode,
                 ShowReplaceSuggestionPrompt, SubmitReplaceSuggestion, HideReplaceSuggestionPrompt,
                 ForgetLastCommit, ToggleDebugWindow>;

}  // namespace azookey::core
