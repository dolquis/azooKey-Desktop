#pragma once

#include <Windows.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace azookey::tsf::ime_switch {

// One entry of the language's profile list: a TSF text service profile or a
// plain keyboard layout (clsid == GUID_NULL).
struct ProfileEntry {
  CLSID clsid{};
  GUID profile{};
  LANGID lang{0};
  HKL hkl{nullptr};
  bool is_layout{false};
  bool keyboard{true};  // A TIP in GUID_TFCAT_TIP_KEYBOARD (always true for layouts).
  bool enabled{false};
};

// The TSF calls the switch needs, behind an interface so the decision logic is
// testable without a live TSF session.
class ProfileApi {
 public:
  virtual ~ProfileApi() = default;
  // The profile the keyboard category is currently active on, if known.
  virtual std::optional<ProfileEntry> ActiveProfile() = 0;
  virtual HRESULT EnumProfiles(LANGID lang, std::vector<ProfileEntry>* out) = 0;
  // Activates for the whole session (TF_IPPMF_FORSESSION).
  virtual HRESULT Activate(const ProfileEntry& target) = 0;
};

enum class SwitchOutcome {
  Switched,
  AlreadyElsewhere,  // The session is not on azooKey; nothing to do.
  NoTarget,          // No other enabled input method for the language.
  EnumFailed,
  ActivateFailed,
};

// Microsoft Japanese IME, the preferred landing place.
inline constexpr CLSID kMicrosoftJapaneseImeClsid = {
    0x03b5835f, 0xf03c, 0x411b, {0x9c, 0xe2, 0xaa, 0x23, 0xe1, 0x17, 0x1e, 0x22}};

// Rank of a candidate (lower is better); nullopt when unusable.
inline std::optional<int> RankTarget(const ProfileEntry& entry, const CLSID& own_clsid) {
  if (!entry.enabled || !entry.keyboard) return std::nullopt;
  if (entry.is_layout) return 2;
  if (IsEqualGUID(entry.clsid, own_clsid)) return std::nullopt;
  return IsEqualGUID(entry.clsid, kMicrosoftJapaneseImeClsid) ? 0 : 1;
}

inline std::optional<size_t> ChooseTarget(const std::vector<ProfileEntry>& entries,
                                          const CLSID& own_clsid) {
  std::optional<size_t> best;
  int best_rank = 0;
  for (size_t index = 0; index < entries.size(); ++index) {
    const auto rank = RankTarget(entries[index], own_clsid);
    if (!rank) continue;
    if (!best || *rank < best_rank) {
      best = index;
      best_rank = *rank;
    }
  }
  return best;
}

// Moves the session off azooKey before its registration is removed. It never
// disables the azooKey profile: if no switch happens, the user must still have
// an input method, and a per-user "disabled" mark would outlive a re-registration.
inline SwitchOutcome SwitchAwayFromOwnProfile(ProfileApi& api, const CLSID& own_clsid,
                                              LANGID lang) {
  // The query runs from a focus-less one-shot thread, so a keyboard layout or a
  // failure proves nothing about the other apps' sessions; only a different TIP
  // is positive evidence that the session already left azooKey.
  const auto active = api.ActiveProfile();
  if (active && !active->is_layout && !IsEqualGUID(active->clsid, own_clsid)) {
    return SwitchOutcome::AlreadyElsewhere;
  }

  std::vector<ProfileEntry> entries;
  if (FAILED(api.EnumProfiles(lang, &entries))) return SwitchOutcome::EnumFailed;
  const auto chosen = ChooseTarget(entries, own_clsid);
  if (!chosen) return SwitchOutcome::NoTarget;
  if (FAILED(api.Activate(entries[*chosen]))) return SwitchOutcome::ActivateFailed;
  return SwitchOutcome::Switched;
}

inline int ExitCodeFor(SwitchOutcome outcome) {
  switch (outcome) {
    case SwitchOutcome::Switched:
    case SwitchOutcome::AlreadyElsewhere:
      return 0;
    default:
      return 1;
  }
}

}  // namespace azookey::tsf::ime_switch
