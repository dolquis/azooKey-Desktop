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
  // The commit checks still_current under its own lock before changing anything and
  // returns Cancelled when it is false; otherwise Ingested, or Failed when not saved.
  std::function<TrendingFetchStatus(const std::vector<learning::AutoWord>&, uint64_t, bool,
                                    const std::function<bool()>& still_current)>
      ingest_and_save;
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

  // Cancels the old generation without waiting for its network request or its
  // cache/store I/O. The commit rechecks the generation under its own lock, which is
  // the linearization point: a commit past it is ordered before this call and may
  // finish persisting after this returns; any other commit of the old generation is
  // cancelled. Only the public cache file may still be refreshed by a cancelled one.
  void UpdateSettings(bool enabled, uint32_t interval_hours, bool auto_promote);
  // Start/Stop are serialized by the owner, not called from report callbacks.
  // Stop waits for a commit in progress, so the store may be destroyed after it.
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
  // Serializes fetches and their cache/store I/O; Stop takes it to wait for them.
  std::mutex fetch_mutex_;
  // Guards only the settings below; never held across I/O.
  std::mutex settings_mutex_;
  std::condition_variable wake_;
  bool enabled_{false};
  bool auto_promote_{false};
  uint32_t interval_hours_{24};
  std::atomic<uint64_t> generation_{0};
  std::atomic<bool> stopped_{false};
  std::thread worker_;
  // Set only after Save succeeds; a failed save is retried on the next fetch.
  // Guarded by fetch_mutex_.
  std::string ingested_generated_at_;
};

}  // namespace azookey::host
