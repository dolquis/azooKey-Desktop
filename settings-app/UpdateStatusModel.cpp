#include "UpdateStatusModel.h"

#include <algorithm>
#include <fstream>
#include <system_error>

#include "azookey/ipc/Json.h"

namespace azookey::settings {
namespace {

constexpr uintmax_t kMaxStatusBytes = 64 * 1024;
constexpr size_t kMaxText = 512;

std::string Bounded(const std::optional<std::string>& value) {
  return value && value->size() <= kMaxText ? *value : std::string();
}

bool IsReleasePage(std::string_view url) {
  if (url.size() <= kReleasePagePrefix.size() || url.size() > kMaxText ||
      !url.starts_with(kReleasePagePrefix)) {
    return false;
  }
  return std::all_of(url.begin(), url.end(), [](char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte > 0x20 && byte < 0x7f && c != '"' && c != '<' && c != '>' && c != '\\';
  });
}

}  // namespace

UpdateStatusView ParseUpdateStatus(std::string_view json) {
  UpdateStatusView view;
  const auto root = azookey::ipc::json::Parse(json);
  if (!root || !root->IsObject() || root->GetUInt("version") != 1u) return view;
  const auto result = root->GetString("result");
  if (result == "update_available") {
    view.kind = UpdateCheckKind::UpdateAvailable;
  } else if (result == "up_to_date") {
    view.kind = UpdateCheckKind::UpToDate;
  } else if (result == "failed") {
    view.kind = UpdateCheckKind::Failed;
  } else {
    return view;
  }
  view.channel = Bounded(root->GetString("channel"));
  view.current_version = Bounded(root->GetString("currentVersion"));
  view.latest_version = Bounded(root->GetString("latestVersion"));
  const auto url = Bounded(root->GetString("releaseUrl"));
  if (IsReleasePage(url)) view.release_url = url;
  view.checked_at_epoch = root->GetUInt("checkedAtEpoch");
  // An available update is shown only with the version it names.
  if (view.kind == UpdateCheckKind::UpdateAvailable && view.latest_version.empty()) {
    view.kind = UpdateCheckKind::None;
  }
  return view;
}

UpdateStatusView ReadUpdateStatus(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size == 0 || size > kMaxStatusBytes) return {};
  std::ifstream input(path, std::ios::binary);
  std::string text(static_cast<size_t>(size), '\0');
  if (!input.read(text.data(), static_cast<std::streamsize>(size))) return {};
  return ParseUpdateStatus(text);
}

std::string UpdateStatusResource(const UpdateStatusView& status, bool enabled) {
  if (!enabled) return "UpdateStatus_Disabled";
  switch (status.kind) {
    case UpdateCheckKind::UpdateAvailable:
      return "UpdateStatus_UpdateAvailable";
    case UpdateCheckKind::UpToDate:
      return "UpdateStatus_UpToDate";
    case UpdateCheckKind::Failed:
      return "UpdateStatus_Failed";
    case UpdateCheckKind::None:
      break;
  }
  return "UpdateStatus_None";
}

}  // namespace azookey::settings
