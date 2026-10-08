#include "NeologdAttribution.h"

#include <utility>

#include "azookey/ipc/Json.h"

namespace azookey::settings {

std::optional<NeologdPackAttribution> ParseNeologdPackAttribution(std::string_view manifest_json) {
  const auto parsed = azookey::ipc::json::Parse(manifest_json);
  if (!parsed || !parsed->IsObject()) return std::nullopt;
  if (parsed->GetString("pack_id") != "neologd_lexicon") return std::nullopt;
  const auto url = parsed->GetString("url");
  const auto revision = parsed->GetString("upstream_revision");
  const auto* attribution = parsed->Find("attribution");
  if (!url || !revision || !attribution || !attribution->IsObject()) return std::nullopt;
  auto notices = attribution->GetString("notices");
  if (!notices || notices->empty()) return std::nullopt;

  NeologdPackAttribution result;
  result.notices = std::move(*notices);
  result.upstream_revision = *revision;
  // Same rule as the Host's NeologdPackManifest::published().
  result.published = !url->empty();
  return result;
}

bool NeologdConsentRequired(bool previously_enabled, bool requested_enabled) {
  return !previously_enabled && requested_enabled;
}

}  // namespace azookey::settings
