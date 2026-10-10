#include "azookey/host/CandidateTags.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "azookey/core/Utf8.h"

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
  for (size_t i = 0; i < surface.size(); ++i) {
    const auto byte = static_cast<unsigned char>(surface[i]);
    if ((byte & 0xC0) == 0x80) continue;  // UTF-8 continuation byte.
    if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r') continue;
    if (surface.compare(i, 3, "\xE3\x80\x80") == 0) continue;  // U+3000 ideographic space.
    ++code_points;
    if (byte < 0x80) {
      ++ascii;
      if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')) has_ascii_letter = true;
    }
  }
  return has_ascii_letter && ascii * 2 > code_points;
}

bool IsTrailingStyleSeparator(char32_t cp) {
  // Unicode White_Space plus only the punctuation allowed by app-profile-spec section 7.
  return (cp >= 0x9 && cp <= 0xd) || cp == U' ' || cp == 0x85 || cp == 0xa0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f ||
         cp == 0x205f || cp == 0x3000 || cp == U'。' || cp == U'！' || cp == U'!' || cp == U'？' ||
         cp == U'?';
}

core::CandidateTag HeuristicStyleTag(std::string_view surface) {
  size_t offset = 0;
  size_t end = 0;
  char32_t cp = 0;
  while (offset < surface.size()) {
    if (!core::DecodeNextUtf8(surface, offset, cp)) return core::CandidateTag::None;
    if (!IsTrailingStyleSeparator(cp)) end = offset;
  }
  surface = surface.substr(0, end);
  // Keep all approved suffixes from app-profile-spec section 7 explicit, even
  // when a longer suffix also ends with a shorter one in this table.
  static constexpr std::string_view kPoliteSuffixes[] = {
      "です",         "ます",       "でした",       "ました",  "ません",
      "ませんでした", "ございます", "ございました", "ください"};
  for (const auto suffix : kPoliteSuffixes) {
    if (surface.ends_with(suffix)) return core::CandidateTag::Polite;
  }
  static constexpr std::string_view kCasualSuffixes[] = {"だよ", "だね", "だぞ",
                                                         "だぜ", "だろ", "じゃん"};
  for (const auto suffix : kCasualSuffixes) {
    if (surface.ends_with(suffix)) return core::CandidateTag::Casual;
  }
  return core::CandidateTag::None;
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
    if (candidate.tag != core::CandidateTag::None) continue;
    candidate.tag = HeuristicStyleTag(candidate.surface);
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
