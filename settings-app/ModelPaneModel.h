#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "SettingsIpcClient.h"
#include "azookey/ipc/Payloads.h"

namespace azookey::settings {

// How the "モデル" pane shows one ListModels entry (model-management-spec section 6.2).
enum class ModelEntryState {
  Loaded,      // Valid, and the running engine has loaded it.
  NotLoaded,   // Valid, not loaded so far.
  LoadFailed,  // Valid, but loading it failed.
  Invalid,     // Failed validation.
};

ModelEntryState ClassifyModel(const azookey::ipc::ListedModel& model);

// Only a valid GGUF file can be chosen: model.selectedPath is a file path, and saving rejects
// anything else (sideload-packaging-spec section 3.6).
bool IsSelectableModel(const azookey::ipc::ListedModel& model);

// Whether two UTF-8 paths name the same file, ignoring case and redundant separators.
bool SameModelPath(std::string_view left, std::string_view right);

// "1.2 GB"; one decimal above 1 KB, whole bytes below.
std::string FormatByteSize(uint64_t bytes);

// Error categories a ListModels / BenchmarkModel response can carry
// (model-management-spec sections 4.1 and 4.2). Each has a "ModelsError_<code>" string; any other
// category falls back to "ModelsError_other".
inline constexpr std::array<std::string_view, 12> kModelErrorCodes{"invalid_request",
                                                                   "models_dir_unavailable",
                                                                   "directory_outside_models_root",
                                                                   "scan_failed",
                                                                   "not authenticated",
                                                                   "unsupported_backend",
                                                                   "invalid_model",
                                                                   "path_outside_models_root",
                                                                   "busy",
                                                                   "load_failed",
                                                                   "safe_mode",
                                                                   "benchmark_failed"};

std::string ModelErrorResource(std::string_view error);

// The string for a request that produced no Host response at all.
std::string HostCallStatusResource(HostCallStatus status);

// DEV-1821 catalog fetch (model-management-spec sections 3.1.2 and 4.4). Error categories a
// FetchCatalogModel / QueryCatalogFetch / CancelCatalogFetch (DEV-1840) response can carry, as
// the request's error or the fetch's result_error. Each has a "CatalogFetchError_<code>"
// string; "format:<category>" uses "CatalogFetchError_format", and any other category
// "CatalogFetchError_other".
inline constexpr std::array<std::string_view, 11> kCatalogFetchErrorCodes{"invalid_request",
                                                                          "busy",
                                                                          "unknown_model_id",
                                                                          "catalog_unavailable",
                                                                          "not authenticated",
                                                                          "directory_unavailable",
                                                                          "destination_occupied",
                                                                          "download_failed",
                                                                          "cancelled",
                                                                          "format",
                                                                          "not_running"};

std::string CatalogFetchErrorResource(std::string_view error);

// The string for a finished fetch: what happened, or why it failed.
std::string CatalogFetchResultResource(const azookey::ipc::CatalogFetchResponse& response);

// A finished fetch whose file can go into model.selectedPath: the SHA-256 matched the pin and
// the GGUF passed the R1 checks (sideload-packaging-spec section 1.6.1, the probe).
bool CatalogFetchSucceeded(const azookey::ipc::CatalogFetchResponse& response);

// Whether the entry's fetch button can start a fetch: a missing entry with a source, while no
// fetch runs and no stop this window sent is still unanswered (DEV-1840). The Host matches a
// stop by model id, so a fetch of the same entry started before the answer could be the one
// the stop reaches.
bool CanStartCatalogFetch(const azookey::ipc::CatalogModelEntry& entry, bool fetching,
                          bool cancel_in_flight);

// The string for a catalog entry's file state ("missing", "verified", "mismatch").
std::string CatalogEntryStateResource(std::string_view state);

// Whole percent of received out of total, capped at 100; 0 when total is unknown.
uint32_t ProgressPercent(uint64_t received, uint64_t total);

// How long the received byte count may stay still before the settings app stops waiting for a
// running fetch. Longer than the Host's receive timeout, and than hashing a large file.
inline constexpr std::chrono::milliseconds kCatalogFetchStallTimeout{120000};

// Tells a stalled fetch from a slow one: only the time since received_bytes last grew counts,
// not the whole download.
class CatalogFetchStallWatch {
 public:
  using Clock = std::chrono::steady_clock;

  explicit CatalogFetchStallWatch(Clock::time_point start,
                                  std::chrono::milliseconds limit = kCatalogFetchStallTimeout)
      : limit_(limit), last_change_(start) {}

  // Records one poll; true once received has not grown for longer than the limit.
  bool Stalled(uint64_t received, Clock::time_point now);

 private:
  std::chrono::milliseconds limit_;
  Clock::time_point last_change_;
  uint64_t received_{0};
};

}  // namespace azookey::settings
