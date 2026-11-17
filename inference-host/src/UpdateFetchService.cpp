#include "azookey/host/UpdateFetchService.h"

#include <fstream>
#include <optional>
#include <system_error>
#include <utility>

#include "azookey/ipc/Json.h"
#include "azookey/ipc/Limits.h"
#include "azookey/logging/RuntimeLogger.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace azookey::host {
namespace {

constexpr uint64_t kMaxStatusBytes = 64 * 1024;

// The values reaching here are validated ASCII (a version, or a URL built from fixed text and
// one), so a byte-wise widening is exact.
std::wstring WidenAscii(std::string_view text) { return std::wstring(text.begin(), text.end()); }

// Only the files this service writes: <prefix><MAJOR.MINOR.PATCH>-x64.msi and its .part, where the
// prefix is "azooKey-" for the base and "azooKey-vulkan-addon-" for the add-on. Anything else in
// the directory stays.
bool IsPackageFileName(const std::filesystem::path::string_type& name, const char* prefix_text) {
  using View = std::basic_string_view<std::filesystem::path::value_type>;
  const std::filesystem::path prefix_path(prefix_text);
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

void UpdateFetchService::RemoveOtherPackages(const std::string& version) const {
  const std::filesystem::path keep_base(UpdatePackageFileName(version));
  const std::filesystem::path keep_addon(VulkanAddonPackageFileName(version));
  const auto kept = [&](const std::filesystem::path::string_type& name) {
    for (const auto& keep : {keep_base, keep_addon}) {
      auto part = keep;
      part += ".part";
      if (name == keep.native() || name == part.native()) return true;
    }
    return false;
  };
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
    if ((!IsPackageFileName(name, "azooKey-") &&
         !IsPackageFileName(name, "azooKey-vulkan-addon-")) ||
        kept(name)) {
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
  // DEV-2081: the add-on of the same version, only where it is installed. Its failure leaves the
  // base Ready (addon_error); a stop by the user, the Host or the settings stops the whole job.
  bool addon_wanted = false;
  std::string addon_sha256;
  std::string addon_error;
  try {
    addon_wanted = dependencies_.vulkan_addon_installed && dependencies_.vulkan_addon_installed();
    HttpTextRequest request;
    request.url = options_.api_base_url + L"/releases/tags/v" + WidenAscii(version);
    request.max_bytes = ipc::kMaxJsonInputBytes;
    request.cancelled = cancelled;
    request.network_allowed = allowed;
    const auto response = dependencies_.fetch_text(request);
    std::optional<UpdatePackage> package;
    std::optional<UpdatePackage> addon;
    if (!response.ok()) {
      error = category("network");
    } else {
      std::string select_error;
      package = SelectUpdatePackage(response.body, version, options_.download_prefix,
                                    options_.max_bytes, &select_error);
      if (!package) error = select_error;
      if (package && addon_wanted) {
        addon = SelectVulkanAddonPackage(response.body, version, options_.download_prefix,
                                         options_.max_bytes, &addon_error);
      }
    }
    if (package) {
      std::error_code ec;
      std::filesystem::create_directories(options_.updates_dir, ec);
      if (ec) {
        error = "save_failed";
        package.reset();
      }
    }
    // The progress counts both files: the add-on's bytes follow the base's.
    const uint64_t addon_size = addon ? addon->size : 0;
    const auto fetch = [&](const UpdatePackage& file, uint64_t offset) {
      HttpDownloadRequest download;
      download.url = WidenAscii(file.download_url);
      download.destination = options_.updates_dir / file.file_name;
      download.expected_sha256 = file.sha256;
      download.max_bytes = file.size;
      download.cancelled = cancelled;
      download.network_allowed = allowed;
      const uint64_t after = offset == 0 ? addon_size : 0;
      download.progress = [this, offset, after](uint64_t received, uint64_t total) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.received_bytes = offset + received;
        if (total != 0) snapshot_.total_bytes = offset + total + after;
      };
      auto result = dependencies_.download(download);
      bytes_received += result.bytes_received;
      return result;
    };
    if (package) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.total_bytes = package->size + addon_size;
      }
      if (fetch(*package, 0).ok()) {
        sha256 = package->sha256;
        RemoveOtherPackages(version);
      } else {
        error = category("download_failed");
      }
    }
    if (error.empty() && addon) {
      if (fetch(*addon, package->size).ok()) {
        addon_sha256 = addon->sha256;
      } else {
        addon_error = category("download_failed");
        // A stop ends the job; any other failure of the add-on leaves the base to install.
        if (addon_error == "cancelled" || addon_error == "disabled") error = addon_error;
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
    if (addon_wanted) {
      snapshot_.addon_sha256 = addon_sha256;
      snapshot_.addon_error = addon_sha256.empty() ? addon_error : std::string();
    }
  } else {
    snapshot_.state = UpdateFetchSnapshot::State::Failed;
    snapshot_.error = error;
  }
  if (runtime_logger_) {
    // The result, the fixed category and the bytes received only: the URL, the path and the
    // downloader's message stay out of the log.
    const bool stopped = error == "cancelled" || error == "disabled";
    if (error.empty() && addon_wanted && addon_sha256.empty()) {
      // The base is ready but the add-on is not: a Warn with the add-on's fixed category.
      runtime_logger_->Log(logging::RuntimeLogLevel::Warn, "update_fetch",
                           {{"result", logging::RuntimeLogSafeText("ready")},
                            {"reason", logging::RuntimeLogSafeText("verified")},
                            {"addon_reason", logging::RuntimeLogSafeText(addon_error)},
                            {"error_code", logging::RuntimeLogSafeText("business")},
                            {"bytes_received", bytes_received}});
    } else if (error.empty() && addon_wanted) {
      runtime_logger_->Log(logging::RuntimeLogLevel::Info, "update_fetch",
                           {{"result", logging::RuntimeLogSafeText("ready")},
                            {"reason", logging::RuntimeLogSafeText("verified")},
                            {"addon_reason", logging::RuntimeLogSafeText("verified")},
                            {"bytes_received", bytes_received}});
    } else if (error.empty() || stopped) {
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

bool VulkanAddonInstalled() {
#ifdef _WIN32
  // The add-on MSI writes the key from a 64-bit component (pkg/msi-vulkan/VulkanAddon.wxs).
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\azooKey\\VulkanAddon", 0,
                    KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return false;
  }
  RegCloseKey(key);
  return true;
#else
  return false;
#endif
}

void FillUpdateFetchResponse(const UpdateFetchSnapshot& snapshot,
                             ipc::UpdateFetchResponse& response) {
  response.state = std::string(UpdateFetchStateName(snapshot.state));
  response.version = snapshot.version;
  response.received_bytes = snapshot.received_bytes;
  response.total_bytes = snapshot.total_bytes;
  response.sha256.clear();
  response.result_error.clear();
  response.addon_sha256.clear();
  response.addon_error.clear();
  if (snapshot.state == UpdateFetchSnapshot::State::Ready) {
    response.sha256 = snapshot.sha256;
    response.addon_sha256 = snapshot.addon_sha256;
    response.addon_error = snapshot.addon_error;
  }
  if (snapshot.state == UpdateFetchSnapshot::State::Failed) response.result_error = snapshot.error;
}

}  // namespace azookey::host
