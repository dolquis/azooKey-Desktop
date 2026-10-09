#pragma once

#include <array>
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

}  // namespace azookey::settings
