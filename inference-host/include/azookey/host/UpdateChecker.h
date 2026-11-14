#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "azookey/host/HttpDownloader.h"

namespace azookey::host {

// M32 update check (docs/sideload-packaging-spec.md section 6). It finds a newer release,
// records it for the settings app and asks for one notification per version; it never
// downloads or installs one.

enum class UpdateCheckStatus { Disabled, UpToDate, UpdateAvailable, Failed, Cancelled };

// MAJOR.MINOR.PATCH with an optional pre-release (SemVer 2.0.0 precedence).
struct ReleaseVersion {
  uint64_t major{0};
  uint64_t minor{0};
  uint64_t patch{0};
  std::vector<std::string> prerelease;
};

// Accepts an optional leading 'v' (release tags are v*). nullopt for anything else, including
// build metadata ('+'), which would otherwise be shown without being compared.
std::optional<ReleaseVersion> ParseReleaseVersion(std::string_view text);
// Negative, zero or positive as a has lower, equal or higher precedence than b.
int CompareReleaseVersions(const ReleaseVersion& a, const ReleaseVersion& b);

// The repository's release pages: the only pages the check records and a notification opens.
inline constexpr std::string_view kReleasePagePrefix =
    "https://github.com/dolquis/azooKey-Desktop/releases/";

// Whether url is a page under prefix without dot segments, escapes, spaces or characters that
// need quoting, which a browser could resolve to a page outside the prefix.
bool IsAllowedReleaseUrl(std::string_view url, std::string_view prefix);

struct AvailableRelease {
  std::string version;  // As tagged, without the leading 'v'.
  std::string url;      // The release page; only https://github.com/<repo>/releases/... is kept.
};

// Picks the release a channel offers from a GitHub Releases API response: for "stable" the
// object from /releases/latest, for "beta" the array from /releases (drafts skipped,
// pre-releases included, highest version wins). nullopt when the response is not usable.
std::optional<AvailableRelease> SelectRelease(std::string_view json, std::string_view channel,
                                              std::string_view release_page_prefix);

struct UpdateCheckerOptions {
  // This build's product version (AZOOKEY_PRODUCT_VERSION).
  std::string current_version;
  // GitHub Releases API of the repository, without a trailing '/'. Tests point it at loopback.
  std::wstring api_base_url{L"https://api.github.com/repos/dolquis/azooKey-Desktop"};
  // Release pages outside this prefix are dropped, so the settings app opens only them.
  std::string release_page_prefix{kReleasePagePrefix};
  // update-status.json, written atomically after each finished check.
  std::filesystem::path status_path;
  // update-notification.json: the highest version notified, so a version is notified once and
  // only a higher one is notified after it. Empty: never notify.
  std::filesystem::path notification_path;
};

struct UpdateCheckDependencies {
  std::function<HttpTextResult(const HttpTextRequest&)> fetch_text;
  // Wall clock in seconds since the epoch; spaces checks across Host restarts.
  std::function<uint64_t()> now_epoch;
  // Monotonic clock for the wait between checks. Overrides must be thread-safe.
  std::function<std::chrono::steady_clock::time_point()> steady_now;
  // False while the privacy setting forbids the check whatever the config says, for example
  // after a reload found settings.json invalid (fail closed; no config is applied then). Read
  // on the worker and while a check waits; must be thread-safe. Empty allows the check.
  std::function<bool()> network_allowed;
  // Invoked on the worker with the status only (no URL or response text).
  std::function<void(UpdateCheckStatus)> report;
  // Invoked on the worker after a check recorded a newer release that has a release page and is
  // above the version recorded in notification_path. True when the notification was shown: only
  // then the version is recorded, so a failed or suppressed notification is tried at the next
  // check.
  std::function<bool(const AvailableRelease&)> notify_update;
};

// Owns one worker. The request carries only the fixed API URL and a User-Agent with the
// product version: no identifier, settings or usage data.
class UpdateChecker {
 public:
  explicit UpdateChecker(UpdateCheckerOptions options, UpdateCheckDependencies dependencies = {});
  ~UpdateChecker();

  // enabled must already be false under privacy.mode=offline. A change cancels a check in
  // progress without waiting for it, and wakes the worker.
  void UpdateSettings(bool enabled, std::string channel, uint32_t interval_hours);
  // Start/Stop are serialized by the owner. Stop waits for the worker.
  void Start();
  void Stop();
  // One check with the current settings, also usable without Start.
  UpdateCheckStatus CheckOnce();

 private:
  void Run();
  UpdateCheckStatus CheckOnceImpl();
  // checkedAtEpoch of the last finished check for this channel recorded in status_path.
  std::optional<uint64_t> LastCheckedEpoch(const std::string& channel) const;
  // Notifies a newer release once per version (notification_path).
  void NotifyOnce(const AvailableRelease& release);
  std::optional<std::string> NotifiedVersion() const;

  const UpdateCheckerOptions options_;
  UpdateCheckDependencies dependencies_;
  // Serializes checks and their status writes.
  std::mutex check_mutex_;
  // Guards the settings below; never held across I/O.
  std::mutex settings_mutex_;
  std::condition_variable wake_;
  bool enabled_{false};
  std::string channel_{"stable"};
  uint32_t interval_hours_{24};
  std::atomic<uint64_t> generation_{0};
  // Counts UpdateSettings calls, so a worker held back by network_allowed looks again on the
  // next reload even when it leaves the settings unchanged.
  uint64_t settings_calls_{0};
  std::atomic<bool> stopped_{false};
  std::thread worker_;
};

}  // namespace azookey::host
