#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "azookey/host/HttpDownloader.h"
#include "azookey/learning/AutoWordStore.h"

namespace azookey::host {

enum class TrendingFetchStatus {
  Disabled,
  SourceUnavailable,
  Unchanged,
  Ingested,
  Failed,
  Cancelled
};

struct TrendingFetchDependencies {
  std::function<HttpTextResult(const HttpTextRequest&)> fetch_text;
  std::function<HttpDownloadResult(const HttpDownloadRequest&)> download;
  std::function<uint64_t()> now_epoch;
  // Monotonic clock for both polling intervals and the 15-second fetch deadline.
  // Production defaults to steady_clock; test overrides must be thread-safe.
  std::function<std::chrono::steady_clock::time_point()> steady_now;
  // Host supplies the engine's serialized commit so reset/forget rollback cannot
  // overwrite a successful ingestion. An isolated store may use the default.
  std::function<bool(const std::vector<learning::AutoWord>&, uint64_t, bool)> ingest_and_save;
  // Invoked on the worker, with no URL, downloaded text or user words.
  std::function<void(TrendingFetchStatus)> report;
};

// Owns one serial worker. The store and dependency captures must outlive it.
// Network requests contain only the fixed public asset URL (and its .sha256).
class TrendingWordFetcher {
 public:
  TrendingWordFetcher(learning::AutoWordStore& store, std::filesystem::path cache_path,
                      std::wstring asset_url, TrendingFetchDependencies dependencies = {});
  ~TrendingWordFetcher();

  // Cancels the old generation without joining its network request. The commit
  // lock prevents an obsolete result from being ingested after this returns.
  void UpdateSettings(bool enabled, uint32_t interval_hours, bool auto_promote);
  // Start/Stop are serialized by the owner, not called from report callbacks.
  void Start();
  void Stop();
  // Also usable without Start, for a local fixture or a manual single attempt.
  TrendingFetchStatus FetchOnce();

 private:
  void Run();
  TrendingFetchStatus FetchOnceImpl();

  learning::AutoWordStore& store_;
  const std::filesystem::path cache_path_;
  const std::wstring asset_url_;
  TrendingFetchDependencies dependencies_;
  std::mutex fetch_mutex_;
  std::mutex settings_mutex_;
  std::condition_variable wake_;
  bool enabled_{false};
  bool auto_promote_{false};
  uint32_t interval_hours_{24};
  std::atomic<uint64_t> generation_{0};
  std::atomic<bool> stopped_{false};
  std::thread worker_;
  // Set only after Save succeeds; a failed save is retried on the next fetch.
  std::string ingested_generated_at_;
};

}  // namespace azookey::host
