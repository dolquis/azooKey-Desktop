#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "azookey/host/ModelCatalog.h"
#include "azookey/host/NeologdPack.h"

namespace azookey::host {

// docs/model-management-spec.md section 3.1.2: the upstream fetch of one catalog entry.
enum class ModelFetchStatus : uint8_t {
  Downloaded,
  // The file at the destination already matched the SHA-256; nothing was fetched.
  AlreadyValid,
  // privacy.mode=offline left no verified copy to use, so nothing was fetched.
  Offline,
  // The entry has no source (a v1 entry, or a v2 artifact without one).
  NotFetchable,
  Failed,
};

struct ModelFetchResult {
  ModelFetchStatus status{ModelFetchStatus::Failed};
  std::filesystem::path path;
  uint64_t bytes_received{0};
  // Fixed category, safe to log: "directory_unavailable", "destination_occupied" (a file of
  // the same name with another SHA-256 is there and is kept), "download_failed", "cancelled"
  // (hooks.cancelled stopped the download), or "format:<section 3.3 category>". Empty unless
  // the status is Failed.
  std::string error;
  // The downloader's own message for a download_failed or a destination_occupied, for the CLI
  // only. May name the URL or a path, so it is never logged.
  std::string detail;

  bool ok() const {
    return status == ModelFetchStatus::Downloaded || status == ModelFetchStatus::AlreadyValid;
  }
};

std::string_view ModelFetchStatusName(ModelFetchStatus status);

struct ModelFetchHooks {
  // Cooperative cancellation, passed to the downloader as HttpDownloadRequest::cancelled.
  std::function<bool()> cancelled;
  // Bytes received so far and the full size, on the calling thread while the download runs.
  // The full size is the response's Content-Length, or the entry's sizeBytes without one.
  std::function<void(uint64_t received, uint64_t total)> progress;
};

// Fetches entry's GGUF from its pinned Hugging Face revision into zenzai_dir/<fileName>. The
// downloader verifies the SHA-256 before the file takes the final name; the GGUF checks of
// section 3.3 run after, and a freshly downloaded file that fails them is removed. A file that
// already matched the SHA-256 is never removed. One that does not match when the fetch starts
// is not replaced (destination_occupied, before any network operation); the check is made
// only then, so a file placed while the download runs can still be replaced when the
// downloader promotes its result. model.selectedPath is not changed.
// network_allowed is passed to the downloader as HttpDownloadRequest::network_allowed.
ModelFetchResult FetchCatalogModel(const ModelCatalogEntry& entry,
                                   const std::filesystem::path& zenzai_dir,
                                   IPackDownloader& downloader,
                                   std::function<bool()> network_allowed = {},
                                   const ModelFetchHooks& hooks = {});

}  // namespace azookey::host
