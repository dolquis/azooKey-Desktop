#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace azookey::settings {

// Attribution of the opt-in neologd_lexicon pack (auto-word-registration-spec section 15.14).
// The text comes from the pinned manifest, so the UI keeps no copy of its own.
struct NeologdPackAttribution {
  std::string notices;
  std::string upstream_revision;
  bool published{false};
};

// The pinned dictbuild/packs/neologd_lexicon.manifest.json, embedded at build time by
// settings-app/EmbedNeologdPin.cmake (no neologd_lexicon.* file ships with the app).
std::string_view PinnedNeologdPackManifestJson();

std::optional<NeologdPackAttribution> ParseNeologdPackAttribution(std::string_view manifest_json);

// Turning the layer on is the only opt-in, so the notices must be accepted on that transition.
bool NeologdConsentRequired(bool previously_enabled, bool requested_enabled);

}  // namespace azookey::settings
