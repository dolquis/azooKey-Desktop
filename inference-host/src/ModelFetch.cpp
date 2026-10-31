#include "azookey/host/ModelFetch.h"

#include <atomic>
#include <memory>
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
                                   std::function<bool()> network_allowed) {
  ModelFetchResult result;
  const auto url = ModelCatalogDownloadUrl(entry);
  if (!url || entry.size_bytes == 0) {
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

  const auto downloaded = downloader.Download(request);
  result.bytes_received = downloaded.bytes_received;
  if (!downloaded.ok()) {
    if (*refused) {
      result.status = ModelFetchStatus::Offline;
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
