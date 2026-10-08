#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "azookey/core/Candidate.h"
#include "azookey/ipc/Json.h"

namespace azookey::host {

// Per-tag multipliers from a resolved app profile (docs/app-profile-spec.md
// sections 4 and 7). Every multiplier is within [1.0, 3.0]; 1.0 is a no-op.
struct TagBoosts {
  static constexpr size_t kTagCount = 7;  // core::CandidateTag None..Idiom.
  std::array<double, kTagCount> multipliers{1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};

  double For(core::CandidateTag tag) const;
  bool IsNeutral() const;
};

// Case-insensitive ASCII match of a profile tag name ("Technical",
// "technical"). Unknown names and "None" yield nullopt.
std::optional<core::CandidateTag> CandidateTagFromName(std::string_view name);

// Builds the boosts from AppProfileResolver::Resolve output. style and
// preferTechnicalTerms imply kImplicitTagBoost for their tag; an explicit
// candidateTagBoosts entry wins when larger. Malformed values are ignored.
inline constexpr double kImplicitTagBoost = 1.5;
TagBoosts TagBoostsFromProfile(const ipc::json::Object& resolved_profile);

// Tags the candidates that have none from their surface form: a surface made
// mostly of ASCII (with at least one ASCII letter) is English. Dictionary
// category tags (Technical) are assigned by their sources and are kept.
void AssignHeuristicTags(std::vector<core::Candidate>& candidates);

// Raises each boosted candidate's score by its multiplier and moves it up past
// the candidates now scoring below it. Scores may be negative
// (log-probabilities), so a negative score is divided rather than multiplied;
// either way a candidate only moves up, and the others keep their order.
void ApplyTagBoosts(std::vector<core::Candidate>& candidates, const TagBoosts& boosts);

}  // namespace azookey::host
