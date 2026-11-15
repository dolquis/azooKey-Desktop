#include "azookey/host/UpdateFetchService.h"

#include <fstream>
#include <optional>
#include <system_error>
#include <utility>

#include "azookey/ipc/Json.h"
#include "azookey/ipc/Limits.h"
#include "azookey/logging/RuntimeLogger.h"

namespace azookey::host {
namespace {

constexpr uint64_t kMaxStatusBytes = 64 * 1024;

// The values reaching here are validated ASCII (a version, or a URL built from fixed text and
// one), so a byte-wise widening is exact.
std::wstring WidenAscii(std::string_view text) { return std::wstring(text.begin(), text.end()); }

// Only the files this service writes: azooKey-<MAJOR.MINOR.PATCH>-x64.msi and its .part. The
// Vulkan add-on's MSI and anything else in the directory stay.
bool IsPackageFileName(const std::filesystem::path::string_type& name) {
  using View = std::basic_string_view<std::filesystem::path::value_type>;
  const std::filesystem::path prefix_path("azooKey-");
  const std::filesystem::path msi("-x64.msi");
  const std::filesystem::path part("-x64.msi.part");
  const View prefix(prefix_path.native());
  for (const View suffix : {View(msi.native()), View(part.native())}) {
    if (name.size() <= prefix.size() + suffix.size() || !name.starts_with(prefix) ||
        !name.ends_with(suffix)) {
      continue;
    }
    const View middle =
        View(name).substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    std::string version;
    for (const auto c : middle) {
      if (c > 0x7f) return false;
      version.push_back(static_cast<char>(c));
    }
    if (IsInstallableVersion(version)) return true;
  }
  return false;
}

}  // namespace

std::string_view UpdateFetchStateName(UpdateFetchSnapshot::State state) {
  switch (state) {
    case UpdateFetchSnapshot::State::Running:
      return "running";
    case UpdateFetchSnapshot::State::Ready:
      return "ready";
    case UpdateFetchSnapshot::State::Failed:
      return "failed";
    case UpdateFetchSnapshot::State::Idle:
      break;
  }
  return "idle";
}

UpdateFetchService::UpdateFetchService(UpdateFetchOptions options,
                                       UpdateFetchDependencies dependencies,
                                       logging::RuntimeLogger* runtime_logger)
    : options_(std::move(options)),
      dependencies_(std::move(dependencies)),
      runtime_logger_(runtime_logger) {
  // The session's User-Agent is the only client detail the requests carry (section 6.2).
  const auto agent = L"azooKey-Desktop/" + WidenAscii(options_.current_version);
  if (!dependencies_.fetch_text) {
    dependencies_.fetch_text = [agent](const HttpTextRequest& request) {
      return HttpDownloader(agent).FetchText(request);
    };
  }
  if (!dependencies_.download) {
    dependencies_.download = [agent](const HttpDownloadRequest& request) {
      return HttpDownloader(agent).Download(request);
    };
  }
}

UpdateFetchService::~UpdateFetchService() { Shutdown(); }

bool UpdateFetchService::RecordedAsAvailable(std::string_view version) const {
  std::error_code ec;
  const auto size = std::filesystem::file_size(options_.status_path, ec);
  if (ec || size == 0 || size > kMaxStatusBytes) return false;
  std::ifstream input(options_.status_path, std::ios::binary);
  std::string text(static_cast<size_t>(size), '\0');
  if (!input.read(text.data(), static_cast<std::streamsize>(size))) return false;
  const auto root = ipc::json::Parse(text);
  if (!root || !root->IsObject() || root->GetUInt("version") != 1u ||
      root->GetString("result") != "update_available" ||
      root->GetString("latestVersion") != std::string(version) ||
      root->GetString("currentVersion") != options_.current_version) {
    return false;
  }
  return !dependencies_.channel || root->GetString("channel") == dependencies_.channel();
}

UpdateFetchStartResult UpdateFetchService::Start(std::string_view version,
                                                 UpdateFetchSnapshot* state) {
  // The version arrives from a pipe client: only the form an MSI is named with goes further.
  const bool installable = IsInstallableVersion(version);
  const bool allowed = !dependencies_.allowed || dependencies_.allowed();
  const bool recorded = installable && allowed && RecordedAsAvailable(version);
  std::thread finished;
  auto result = UpdateFetchStartResult::Started;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      result = UpdateFetchStartResult::Unavailable;
    } else if (snapshot_.state == UpdateFetchSnapshot::State::Running) {
      result = UpdateFetchStartResult::Busy;
    } else if (!allowed) {
      result = UpdateFetchStartResult::Disabled;
    } else if (!recorded) {
      result = UpdateFetchStartResult::NotAvailable;
    } else {
      const auto previous = snapshot_;
      snapshot_ = UpdateFetchSnapshot{};
      snapshot_.state = UpdateFetchSnapshot::State::Running;
      snapshot_.version = std::string(version);
      try {
        auto stop = std::make_shared<std::atomic<bool>>(false);
        // The new worker waits for this lock before it touches the snapshot.
        std::thread next([this, copy = std::string(version), stop] { Run(copy, stop); });
        // The previous worker has published its end and is only returning; join it outside.
        finished = std::move(worker_);
        worker_ = std::move(next);
        stop_running_ = std::move(stop);
      } catch (...) {
        snapshot_ = previous;
        result = UpdateFetchStartResult::Unavailable;
      }
    }
    if (state) *state = snapshot_;
  }
  if (finished.joinable()) finished.join();
  return result;
}

UpdateFetchSnapshot UpdateFetchService::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

UpdateFetchCancelResult UpdateFetchService::Cancel(std::string_view version,
                                                   UpdateFetchSnapshot* state) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = UpdateFetchCancelResult::NotRunning;
  if (snapshot_.state == UpdateFetchSnapshot::State::Running && snapshot_.version == version &&
      stop_running_) {
    *stop_running_ = true;
    result = UpdateFetchCancelResult::Cancelling;
  }
  if (state) *state = snapshot_;
  return result;
}

void UpdateFetchService::Shutdown() {
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    cancel_ = true;
    worker = std::move(worker_);
  }
  if (worker.joinable()) worker.join();
  std::lock_guard<std::mutex> lock(mutex_);
  runtime_logger_ = nullptr;
}

void UpdateFetchService::RemoveOtherPackages(const std::string& keep) const {
  std::error_code ec;
  // Incremented with an error_code: a failing iteration ends the cleanup instead of throwing.
  for (std::filesystem::directory_iterator it(options_.updates_dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    const auto& entry = *it;
    std::error_code type_ec;
    if (!entry.is_regular_file(type_ec) || type_ec) continue;
    const auto file_name = entry.path().filename();
    const auto& name = file_name.native();
    // Only this feature's own files; a file in use (an install in progress) stays.
    if (!IsPackageFileName(name) || name == std::filesystem::path(keep).native() ||
        name == std::filesystem::path(keep + ".part").native()) {
      continue;
    }
    std::error_code remove_ec;
    std::filesystem::remove(entry.path(), remove_ec);
  }
}

void UpdateFetchService::Run(std::string version, std::shared_ptr<std::atomic<bool>> stop) {
  const auto allowed = [this] { return !dependencies_.allowed || dependencies_.allowed(); };
  const auto cancelled = [this, stop, allowed] {
    return cancel_.load() || stop->load() || !allowed();
  };
  // A stop by the user or the Host, then a stop by the settings, then the step's own category.
  const auto category = [this, stop, allowed](const char* own) -> std::string {
    if (cancel_.load() || stop->load()) return "cancelled";
    if (!allowed()) return "disabled";
    return own;
  };
  std::string error;
  std::string sha256;
  uint64_t bytes_received = 0;
  try {
    HttpTextRequest request;
    request.url = options_.api_base_url + L"/releases/tags/v" + WidenAscii(version);
    request.max_bytes = ipc::kMaxJsonInputBytes;
    request.cancelled = cancelled;
    request.network_allowed = allowed;
    const auto response = dependencies_.fetch_text(request);
    std::optional<UpdatePackage> package;
    if (!response.ok()) {
      error = category("network");
    } else {
      std::string select_error;
      package = SelectUpdatePackage(response.body, version, options_.download_prefix,
                                    options_.max_bytes, &select_error);
      if (!package) error = select_error;
    }
    if (package) {
      std::error_code ec;
      std::filesystem::create_directories(options_.updates_dir, ec);
      if (ec) {
        error = "save_failed";
        package.reset();
      }
    }
    if (package) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.total_bytes = package->size;
      }
      HttpDownloadRequest download;
      download.url = WidenAscii(package->download_url);
      download.destination = options_.updates_dir / package->file_name;
      download.expected_sha256 = package->sha256;
      download.max_bytes = package->size;
      download.cancelled = cancelled;
      download.network_allowed = allowed;
      download.progress = [this](uint64_t received, uint64_t total) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.received_bytes = received;
        if (total != 0) snapshot_.total_bytes = total;
      };
      const auto result = dependencies_.download(download);
      bytes_received = result.bytes_received;
      if (result.ok()) {
        sha256 = package->sha256;
        RemoveOtherPackages(package->file_name);
      } else {
        error = category("download_failed");
      }
    }
  } catch (...) {
    error = "download_failed";
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (error.empty()) {
    snapshot_.state = UpdateFetchSnapshot::State::Ready;
    snapshot_.sha256 = sha256;
    snapshot_.received_bytes = snapshot_.total_bytes;
  } else {
    snapshot_.state = UpdateFetchSnapshot::State::Failed;
    snapshot_.error = error;
  }
  if (runtime_logger_) {
    // The result, the fixed category and the bytes received only: the URL, the path and the
    // downloader's message stay out of the log.
    const bool stopped = error == "cancelled" || error == "disabled";
    if (error.empty() || stopped) {
      runtime_logger_->Log(
          logging::RuntimeLogLevel::Info, "update_fetch",
          {{"result", logging::RuntimeLogSafeText(error.empty() ? "ready" : "failed")},
           {"reason", logging::RuntimeLogSafeText(error.empty() ? std::string("verified") : error)},
           {"bytes_received", bytes_received}});
    } else {
      // A failure carries error_code as update_check and update_notification do.
      runtime_logger_->Log(logging::RuntimeLogLevel::Warn, "update_fetch",
                           {{"result", logging::RuntimeLogSafeText("failed")},
                            {"reason", logging::RuntimeLogSafeText(error)},
                            {"error_code", logging::RuntimeLogSafeText("business")},
                            {"bytes_received", bytes_received}});
    }
  }
}

void FillUpdateFetchResponse(const UpdateFetchSnapshot& snapshot,
                             ipc::UpdateFetchResponse& response) {
  response.state = std::string(UpdateFetchStateName(snapshot.state));
  response.version = snapshot.version;
  response.received_bytes = snapshot.received_bytes;
  response.total_bytes = snapshot.total_bytes;
  response.sha256.clear();
  response.result_error.clear();
  if (snapshot.state == UpdateFetchSnapshot::State::Ready) response.sha256 = snapshot.sha256;
  if (snapshot.state == UpdateFetchSnapshot::State::Failed) response.result_error = snapshot.error;
}

}  // namespace azookey::host
