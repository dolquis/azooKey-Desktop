#include "azookey/host/CandidateTags.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace azookey::host {

namespace {

namespace j = ::azookey::ipc::json;

constexpr double kMinTagBoost = 1.0;
constexpr double kMaxTagBoost = 3.0;

size_t TagIndex(core::CandidateTag tag) { return static_cast<size_t>(tag); }

bool EqualsIgnoreAsciiCase(std::string_view left, std::string_view right) {
  return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](char a, char b) {
    const auto lower = [](char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return lower(a) == lower(b);
  });
}

void RaiseTo(TagBoosts& boosts, core::CandidateTag tag, double multiplier) {
  if (!std::isfinite(multiplier)) return;
  auto& slot = boosts.multipliers[TagIndex(tag)];
  slot = std::max(slot, std::clamp(multiplier, kMinTagBoost, kMaxTagBoost));
}

bool IsAsciiDominant(std::string_view surface) {
  size_t code_points = 0;
  size_t ascii = 0;
  bool has_ascii_letter = false;
  for (const char ch : surface) {
    const auto byte = static_cast<unsigned char>(ch);
    if ((byte & 0xC0) == 0x80) continue;  // UTF-8 continuation byte.
    if (byte == ' ') continue;
    ++code_points;
    if (byte < 0x80) {
      ++ascii;
      if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')) has_ascii_letter = true;
    }
  }
  return has_ascii_letter && ascii * 2 > code_points;
}

double Boosted(double score, double multiplier) {
  return score >= 0.0 ? score * multiplier : score / multiplier;
}

}  // namespace

double TagBoosts::For(core::CandidateTag tag) const {
  const auto index = TagIndex(tag);
  return index < multipliers.size() ? multipliers[index] : 1.0;
}

bool TagBoosts::IsNeutral() const {
  return std::all_of(multipliers.begin(), multipliers.end(), [](double m) { return m == 1.0; });
}

std::optional<core::CandidateTag> CandidateTagFromName(std::string_view name) {
  static constexpr std::pair<std::string_view, core::CandidateTag> kNames[] = {
      {"Polite", core::CandidateTag::Polite},       {"Casual", core::CandidateTag::Casual},
      {"Technical", core::CandidateTag::Technical}, {"English", core::CandidateTag::English},
      {"Kaomoji", core::CandidateTag::Kaomoji},     {"Idiom", core::CandidateTag::Idiom},
  };
  for (const auto& [known, tag] : kNames) {
    if (EqualsIgnoreAsciiCase(name, known)) return tag;
  }
  return std::nullopt;
}

TagBoosts TagBoostsFromProfile(const j::Object& resolved_profile) {
  TagBoosts boosts;
  const j::Value profile(resolved_profile);
  if (const auto style = profile.GetString("style")) {
    if (*style == "polite") RaiseTo(boosts, core::CandidateTag::Polite, kImplicitTagBoost);
    if (*style == "casual") RaiseTo(boosts, core::CandidateTag::Casual, kImplicitTagBoost);
    if (*style == "technical") RaiseTo(boosts, core::CandidateTag::Technical, kImplicitTagBoost);
  }
  if (profile.GetBool("preferTechnicalTerms").value_or(false))
    RaiseTo(boosts, core::CandidateTag::Technical, kImplicitTagBoost);
  if (const auto* explicit_boosts = profile.FindObject("candidateTagBoosts")) {
    for (const auto& [name, value] : *explicit_boosts) {
      const auto tag = CandidateTagFromName(name);
      if (!tag || !value.IsNumber()) continue;  // Unknown tags are forward-compat.
      RaiseTo(boosts, *tag, value.AsNumber());
    }
  }
  return boosts;
}

void AssignHeuristicTags(std::vector<core::Candidate>& candidates) {
  for (auto& candidate : candidates) {
    if (candidate.tag == core::CandidateTag::None && IsAsciiDominant(candidate.surface))
      candidate.tag = core::CandidateTag::English;
  }
}

void ApplyTagBoosts(std::vector<core::Candidate>& candidates, const TagBoosts& boosts) {
  if (boosts.IsNeutral()) return;
  for (size_t i = 0; i < candidates.size(); ++i) {
    const double multiplier = boosts.For(candidates[i].tag);
    if (multiplier == 1.0 || !std::isfinite(candidates[i].score)) continue;
    candidates[i].score = Boosted(candidates[i].score, multiplier);
    for (size_t at = i; at > 0 && candidates[at - 1].score < candidates[at].score; --at)
      std::swap(candidates[at - 1], candidates[at]);
  }
}

}  // namespace azookey::host
