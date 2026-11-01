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
#include <vector>

#include "azookey/host/CatalogModelMatcher.h"
#include "azookey/host/ModelFetch.h"
#include "azookey/host/NeologdPack.h"
#include "azookey/ipc/Payloads.h"

namespace azookey::logging {
class RuntimeLogger;
}

namespace azookey::host {

// docs/model-management-spec.md section 4.4: one catalog fetch at a time for the whole Host,
// run on a background thread so the settings app can ask for its progress with separate
// requests. The last result stays until the next fetch starts.
struct CatalogFetchSnapshot {
  enum class State : uint8_t { Idle, Running, Done };
  State state{State::Idle};
  std::string model_id;
  uint64_t received_bytes{0};
  uint64_t total_bytes{0};
  // Set once Done.
  ModelFetchStatus status{ModelFetchStatus::Failed};
  std::filesystem::path path;
  // ModelFetchResult::error: a fixed category, never the downloader's message.
  std::string error;
};

std::string_view CatalogFetchStateName(CatalogFetchSnapshot::State state);

enum class CatalogFetchStartResult : uint8_t {
  Started,
  Busy,            // Another fetch is running.
  UnknownModelId,  // Not an entry of the catalog.
  Unavailable,     // No catalog, or the service is shutting down.
};

class CatalogFetchService {
 public:
  using DownloaderFactory = std::function<std::unique_ptr<IPackDownloader>()>;

  // network_allowed is false under privacy.mode=offline; a change to false stops a running
  // download (ModelFetch.h). runtime_logger may be null.
  CatalogFetchService(std::shared_ptr<CatalogModelMatcher> matcher, DownloaderFactory downloader,
                      std::function<bool()> network_allowed,
                      logging::RuntimeLogger* runtime_logger = nullptr);
  ~CatalogFetchService();
  CatalogFetchService(const CatalogFetchService&) = delete;
  CatalogFetchService& operator=(const CatalogFetchService&) = delete;

  // An empty id is the catalog's defaultModelId.
  CatalogFetchStartResult Start(std::string_view model_id);
  CatalogFetchSnapshot Snapshot() const;
  // Stops a running download between blocking operations and waits for the thread. Later
  // Start calls return Unavailable. Also run by the destructor.
  void Shutdown();

  const std::shared_ptr<CatalogModelMatcher>& matcher() const { return matcher_; }

 private:
  void Run(ModelCatalogEntry entry);

  const std::shared_ptr<CatalogModelMatcher> matcher_;
  const DownloaderFactory downloader_;
  const std::function<bool()> network_allowed_;
  mutable std::mutex mutex_;
  logging::RuntimeLogger* runtime_logger_;
  CatalogFetchSnapshot snapshot_;
  bool stopping_{false};
  std::atomic<bool> cancel_{false};
  std::thread worker_;
};

// The ListModels "catalog" (model-management-spec section 4.1): every entry with the state of
// its file in the matcher's zenzai directory.
std::vector<ipc::CatalogModelEntry> CatalogEntries(CatalogModelMatcher& matcher);
// Copies the fetch state into a FetchCatalogModel / QueryCatalogFetch response; ok and error,
// which describe the request, are left alone.
void FillCatalogFetchResponse(const CatalogFetchSnapshot& snapshot,
                              ipc::CatalogFetchResponse& response);

}  // namespace azookey::host
