#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "azookey/host/HttpDownloader.h"
#include "azookey/host/UpdateChecker.h"
#include "azookey/ipc/Payloads.h"

namespace azookey::logging {
class RuntimeLogger;
}

namespace azookey::host {

// DEV-1974 (docs/sideload-packaging-spec.md section 6.3): downloads the base MSI of the release
// the update check recorded as available, verified against the SHA-256 digest the GitHub
// Releases API gives for the asset. One download at a time for the whole Host, on a background
// thread; the settings app asks for its progress and installs the verified file itself. The last
// result stays until the next download starts.
struct UpdateFetchSnapshot {
  enum class State : uint8_t { Idle, Running, Ready, Failed };
  State state{State::Idle};
  std::string version;
  uint64_t received_bytes{0};
  uint64_t total_bytes{0};
  // Set once Ready.
  std::string sha256;
  // Set once Failed: a fixed category, never the downloader's message.
  std::string error;
};

std::string_view UpdateFetchStateName(UpdateFetchSnapshot::State state);

enum class UpdateFetchStartResult : uint8_t {
  Started,
  Busy,          // Another download is running.
  NotAvailable,  // Not the version update-status.json records as an available update.
  Disabled,      // autoUpdate.enabled is false, or the privacy mode forbids the network.
  Unavailable,   // The service is shutting down, or no worker could start.
};

enum class UpdateFetchCancelResult : uint8_t {
  Cancelling,  // The running download of that version was asked to stop.
  NotRunning,  // No download runs, or the running one is of another version.
};

struct UpdateFetchOptions {
  // This build's product version (AZOOKEY_PRODUCT_VERSION).
  std::string current_version;
  // GitHub Releases API of the repository, without a trailing '/'. Tests point it at loopback.
  std::wstring api_base_url{L"https://api.github.com/repos/dolquis/azooKey-Desktop"};
  std::string download_prefix{kReleaseDownloadPrefix};
  // update-status.json, read to confirm the version asked for is the available update.
  std::filesystem::path status_path;
  // The directory the MSI is written to: <settings dir>/updates.
  std::filesystem::path updates_dir;
  // The largest MSI accepted.
  uint64_t max_bytes{2ull * 1024 * 1024 * 1024};
};

struct UpdateFetchDependencies {
  std::function<HttpTextResult(const HttpTextRequest&)> fetch_text;
  std::function<HttpDownloadResult(const HttpDownloadRequest&)> download;
  // True while autoUpdate.enabled is on and the privacy mode allows the network (neither
  // offline nor secure). Read at the start and while the download runs; a change to false stops
  // it. Must be thread-safe. Empty allows the download.
  std::function<bool()> allowed;
  // The configured autoUpdate.channel; the recorded check must be of it. Must be thread-safe.
  std::function<std::string()> channel;
};

class UpdateFetchService {
 public:
  // runtime_logger may be null.
  UpdateFetchService(UpdateFetchOptions options, UpdateFetchDependencies dependencies,
                     logging::RuntimeLogger* runtime_logger = nullptr);
  ~UpdateFetchService();
  UpdateFetchService(const UpdateFetchService&) = delete;
  UpdateFetchService& operator=(const UpdateFetchService&) = delete;

  // version must be the MAJOR.MINOR.PATCH update-status.json records as available for this
  // product version and the configured channel. state, when given, receives the download state
  // under the same lock: the download just started, or the one that made it Busy.
  UpdateFetchStartResult Start(std::string_view version, UpdateFetchSnapshot* state = nullptr);
  UpdateFetchSnapshot Snapshot() const;
  // Asks the running download of version to stop between blocking operations; it then ends as
  // failed / "cancelled".
  UpdateFetchCancelResult Cancel(std::string_view version, UpdateFetchSnapshot* state = nullptr);
  // Stops a running download and waits for the thread. Later Start calls return Unavailable.
  // Also run by the destructor.
  void Shutdown();

 private:
  bool RecordedAsAvailable(std::string_view version) const;
  void Run(std::string version, std::shared_ptr<std::atomic<bool>> stop);
  void RemoveOtherPackages(const std::string& keep) const;

  const UpdateFetchOptions options_;
  UpdateFetchDependencies dependencies_;
  mutable std::mutex mutex_;
  logging::RuntimeLogger* runtime_logger_;
  UpdateFetchSnapshot snapshot_;
  bool stopping_{false};
  // The Host's stop (Shutdown), for every download.
  std::atomic<bool> cancel_{false};
  // The user's stop of the running download; a new one for each download.
  std::shared_ptr<std::atomic<bool>> stop_running_;
  std::thread worker_;
};

// Copies the download state into an UpdateFetchResponse; ok and error, which describe the
// request, are left alone.
void FillUpdateFetchResponse(const UpdateFetchSnapshot& snapshot,
                             ipc::UpdateFetchResponse& response);

}  // namespace azookey::host
