#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace azookey::settings {

// The result of the Host's last update check, read from update-status.json beside
// settings.json (sideload-packaging-spec section 6). The settings app only shows it.
enum class UpdateCheckKind {
  // No usable record: none written yet, unreadable, or from an unknown format version.
  None,
  UpdateAvailable,
  UpToDate,
  Failed,
};

struct UpdateStatusView {
  UpdateCheckKind kind{UpdateCheckKind::None};
  std::string channel;
  std::string current_version;
  std::string latest_version;
  // Empty unless it is a release page of the repository (the only link the pane opens).
  std::string release_url;
  std::optional<uint64_t> checked_at_epoch;
};

inline constexpr std::string_view kReleasePagePrefix =
    "https://github.com/dolquis/azooKey-Desktop/releases/";

UpdateStatusView ParseUpdateStatus(std::string_view json);
// None when the file is missing, larger than 64 KiB or invalid.
UpdateStatusView ReadUpdateStatus(const std::filesystem::path& path);

// The resource name of the pane's sentence: "UpdateStatus_<kind>"; "UpdateStatus_Disabled"
// while autoUpdate.enabled is false, "UpdateStatus_Offline" under privacy.mode offline or secure
// (the Host does not check), and "UpdateStatus_None" for a record of another channel or of
// another product version than `product_version` (the record of the build before an update).
// An empty `product_version` is not compared.
std::string UpdateStatusResource(const UpdateStatusView& status, bool enabled, bool offline,
                                 std::string_view channel, std::string_view product_version);

}  // namespace azookey::settings
