#include "azookey/host/ModelFetch.h"

#include <atomic>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

#include "azookey/host/ModelScanner.h"

namespace azookey::host {

std::string_view ModelFetchStatusName(ModelFetchStatus status) {
  switch (status) {
    case ModelFetchStatus::Downloaded:
      return "downloaded";
    case ModelFetchStatus::AlreadyValid:
      return "already_valid";
    case ModelFetchStatus::Offline:
      return "offline";
    case ModelFetchStatus::NotFetchable:
      return "not_fetchable";
    case ModelFetchStatus::Failed:
      break;
  }
  return "failed";
}

ModelFetchResult FetchCatalogModel(const ModelCatalogEntry& entry,
                                   const std::filesystem::path& zenzai_dir,
                                   IPackDownloader& downloader,
                                   std::function<bool()> network_allowed,
                                   const ModelFetchHooks& hooks) {
  ModelFetchResult result;
  const auto url = ModelCatalogDownloadUrl(entry);
  if (!url || entry.size_bytes == 0 || entry.size_bytes > kMaxModelCatalogBytes) {
    result.status = ModelFetchStatus::NotFetchable;
    return result;
  }
  std::error_code ec;
  std::filesystem::create_directories(zenzai_dir, ec);
  if (ec) {
    result.error = "directory_unavailable";
    return result;
  }

  HttpDownloadRequest request;
  // The URL is ASCII: the repo and revision are validated and the file name is percent-encoded.
  request.url.assign(url->begin(), url->end());
  request.destination = zenzai_dir / entry.file_name;
  request.expected_sha256 = entry.sha256;
  request.max_bytes = entry.size_bytes;
  request.cancelled = hooks.cancelled;
  if (hooks.progress) {
    request.progress = [progress = hooks.progress, size = entry.size_bytes](uint64_t received,
                                                                             uint64_t total) {
      progress(received, total != 0 ? total : size);
    };
  }

  // A file of the same name with another SHA-256 is the user's (a manual placement or another
  // version); the fetch does not replace it. The probe forbids the network, so the downloader
  // either reuses a matching file or fails before connecting and leaves the file alone. The
  // check is made once, at the start: HttpDownloader (shared with NeologdPack) promotes with
  // replace, so a file placed while the download runs can still be replaced.
  std::optional<HttpDownloadResult> existing;
  // MSVC reports a missing file through ec as well, so only not_found means "absent"; a
  // status that cannot be read (none) is treated as occupied.
  if (std::filesystem::symlink_status(request.destination, ec).type() !=
      std::filesystem::file_type::not_found) {
    HttpDownloadRequest probe = request;
    probe.network_allowed = [] { return false; };
    probe.progress = {};
    existing = downloader.Download(probe);
    if (!existing->ok()) {
      // Usually another SHA-256; the detail tells a file that could not be read (a sharing
      // violation, for one) apart from it.
      result.error = "destination_occupied";
      result.detail = existing->error.value_or("");
      return result;
    }
  }
  // Records a refusal when it happens, so a later return to normal mode does not turn an
  // offline cancellation into a download failure.
  const auto refused = std::make_shared<std::atomic<bool>>(false);
  if (network_allowed) {
    request.network_allowed = [network_allowed, refused] {
      if (network_allowed()) return true;
      *refused = true;
      return false;
    };
  }

  // A matching file found by the probe is already verified; it is not hashed again.
  const auto downloaded = existing ? *existing : downloader.Download(request);
  result.bytes_received = downloaded.bytes_received;
  if (!downloaded.ok()) {
    if (*refused) {
      result.status = ModelFetchStatus::Offline;
      return result;
    }
    if (hooks.cancelled && hooks.cancelled()) {
      result.error = "cancelled";
      return result;
    }
    result.error = "download_failed";
    result.detail = downloaded.error.value_or("");
    return result;
  }

  const auto inspected = InspectGgufFile(request.destination);
  if (!inspected.valid) {
    // The SHA-256 matched, so the pin itself names a file the Host cannot load.
    if (downloaded.status == HttpDownloadStatus::Downloaded) {
      std::filesystem::remove(request.destination, ec);
    }
    result.error = "format:" + inspected.invalid_reason;
    return result;
  }

  result.status = downloaded.status == HttpDownloadStatus::AlreadyValid
                      ? ModelFetchStatus::AlreadyValid
                      : ModelFetchStatus::Downloaded;
  result.path = request.destination;
  return result;
}

}  // namespace azookey::host
