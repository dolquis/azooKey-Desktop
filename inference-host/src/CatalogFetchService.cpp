#include "azookey/host/CatalogFetchService.h"

#include <utility>

#include "azookey/core/PlatformPaths.h"
#include "azookey/logging/RuntimeLogger.h"

namespace azookey::host {

std::string_view CatalogFetchStateName(CatalogFetchSnapshot::State state) {
  switch (state) {
    case CatalogFetchSnapshot::State::Running:
      return "running";
    case CatalogFetchSnapshot::State::Done:
      return "done";
    case CatalogFetchSnapshot::State::Idle:
      break;
  }
  return "idle";
}

CatalogFetchService::CatalogFetchService(std::shared_ptr<CatalogModelMatcher> matcher,
                                         DownloaderFactory downloader,
                                         std::function<bool()> network_allowed,
                                         logging::RuntimeLogger* runtime_logger)
    : matcher_(std::move(matcher)),
      downloader_(std::move(downloader)),
      network_allowed_(std::move(network_allowed)),
      runtime_logger_(runtime_logger) {}

CatalogFetchService::~CatalogFetchService() { Shutdown(); }

CatalogFetchStartResult CatalogFetchService::Start(std::string_view model_id) {
  if (!matcher_ || !downloader_) return CatalogFetchStartResult::Unavailable;
  const auto& catalog = matcher_->catalog();
  const std::string id(model_id.empty() ? std::string_view(catalog.default_model_id) : model_id);
  const ModelCatalogEntry* entry = nullptr;
  for (const auto& candidate : catalog.models) {
    if (candidate.id == id) {
      entry = &candidate;
      break;
    }
  }
  std::thread finished;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return CatalogFetchStartResult::Unavailable;
    if (snapshot_.state == CatalogFetchSnapshot::State::Running) {
      return CatalogFetchStartResult::Busy;
    }
    if (!entry) return CatalogFetchStartResult::UnknownModelId;
    // The previous worker has published Done and is only returning; join it outside the lock.
    finished = std::move(worker_);
    snapshot_ = CatalogFetchSnapshot{};
    snapshot_.state = CatalogFetchSnapshot::State::Running;
    snapshot_.model_id = entry->id;
    snapshot_.total_bytes = entry->size_bytes;
    worker_ = std::thread([this, copy = *entry] { Run(copy); });
  }
  if (finished.joinable()) finished.join();
  return CatalogFetchStartResult::Started;
}

CatalogFetchSnapshot CatalogFetchService::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

void CatalogFetchService::Shutdown() {
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

void CatalogFetchService::Run(ModelCatalogEntry entry) {
  ModelFetchResult result;
  try {
    const auto downloader = downloader_();
    ModelFetchHooks hooks;
    hooks.cancelled = [this] { return cancel_.load(); };
    hooks.progress = [this](uint64_t received, uint64_t total) {
      std::lock_guard<std::mutex> lock(mutex_);
      snapshot_.received_bytes = received;
      snapshot_.total_bytes = total;
    };
    if (downloader) {
      result =
          FetchCatalogModel(entry, matcher_->zenzai_dir(), *downloader, network_allowed_, hooks);
    } else {
      result.error = "download_failed";
    }
  } catch (...) {
    result = ModelFetchResult{};
    result.error = "download_failed";
  }

  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.state = CatalogFetchSnapshot::State::Done;
  snapshot_.status = result.status;
  snapshot_.path = result.ok() ? result.path : std::filesystem::path();
  snapshot_.error = result.error;
  if (result.ok()) snapshot_.received_bytes = snapshot_.total_bytes;
  if (runtime_logger_) {
    // Section 3.1.2: the id, the status, the fixed category and the bytes received only. The
    // path and the downloader's message can name the user or the URL, so they stay out.
    runtime_logger_->Log(
        result.ok() || result.status == ModelFetchStatus::Offline ||
                result.status == ModelFetchStatus::NotFetchable
            ? logging::RuntimeLogLevel::Info
            : logging::RuntimeLogLevel::Warn,
        "catalog_model_fetch",
        {{"model_id", logging::RuntimeLogSafeText(entry.id)},
         {"result", logging::RuntimeLogSafeText(std::string(ModelFetchStatusName(result.status)))},
         {"reason", logging::RuntimeLogSafeText(result.error)},
         {"bytes_received", result.bytes_received}});
  }
}

std::vector<ipc::CatalogModelEntry> CatalogEntries(CatalogModelMatcher& matcher) {
  std::vector<ipc::CatalogModelEntry> entries;
  const auto& catalog = matcher.catalog();
  entries.reserve(catalog.models.size());
  for (const auto& entry : catalog.models) {
    ipc::CatalogModelEntry out;
    out.id = entry.id;
    out.display_name = entry.display_name;
    out.file_name = entry.file_name;
    out.is_default = entry.id == catalog.default_model_id;
    out.size_bytes = entry.size_bytes;
    out.prompt_format = std::string(ZenzaiPromptFormatName(entry.prompt_format));
    out.fetchable = ModelCatalogDownloadUrl(entry).has_value() && entry.size_bytes != 0 &&
                    entry.size_bytes <= kMaxModelCatalogBytes;
    out.state = std::string(CatalogEntryFileStateName(matcher.StateOf(entry)));
    out.path = core::PathToUtf8((matcher.zenzai_dir() / entry.file_name).lexically_normal());
    entries.push_back(std::move(out));
  }
  return entries;
}

void FillCatalogFetchResponse(const CatalogFetchSnapshot& snapshot,
                              ipc::CatalogFetchResponse& response) {
  response.state = std::string(CatalogFetchStateName(snapshot.state));
  response.model_id = snapshot.model_id;
  response.received_bytes = snapshot.received_bytes;
  response.total_bytes = snapshot.total_bytes;
  response.status.clear();
  response.path.clear();
  response.result_error.clear();
  if (snapshot.state != CatalogFetchSnapshot::State::Done) return;
  response.status = std::string(ModelFetchStatusName(snapshot.status));
  if (!snapshot.path.empty()) response.path = core::PathToUtf8(snapshot.path);
  response.result_error = snapshot.error;
}

}  // namespace azookey::host
