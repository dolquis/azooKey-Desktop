#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "azookey/ipc/Payloads.h"

namespace azookey::host {

// `azookey_inference_host models ...` (M45). --json prints exactly the
// ListModels / BenchmarkModel response payload, so the CLI and the IPC share
// one stable schema (docs/model-management-spec.md section 9).
enum class ModelsCliCommand { List, Bench, Catalog, Fetch };

struct ModelsCliOptions {
  ModelsCliCommand command{ModelsCliCommand::List};
  bool json{false};
  ipc::ListModelsRequest list;
  ipc::BenchmarkModelRequest bench;
  // fetch --id; empty selects the catalog's defaultModelId.
  std::string fetch_model_id;
};

class IPackDownloader;

struct ModelsCliRunOptions {
  std::filesystem::path models_dir;
  // settings.json, read by fetch for privacy.mode only.
  std::filesystem::path settings_path;
  // Tests inject a fake; null uses HttpPackDownloader.
  IPackDownloader* downloader{nullptr};
};

struct ModelsCliResult {
  int exit_code{0};
  std::vector<std::string> output_lines;
  std::string error;
};

// models list [--dir <path>] [--sha256] [--json]
// models bench --path <gguf> [--backend cpu|cuda|vulkan] [--iterations N]
//              [--warmup N] [--case <reading>]... [--json]
// models catalog
//   Lists the embedded default catalog (model-management-spec section 3.1.1).
// models fetch [<model id> | --id <model id>]
//   Fetches the entry from its pinned upstream revision (section 3.1.2). Refused under
//   privacy.mode=offline, when settings.json cannot be read, and when the models directory or
//   the settings path is unknown. Never sets model.selectedPath.
std::optional<ModelsCliOptions> ParseModelsCliArgs(const std::vector<std::string>& args,
                                                   std::string* error);
ModelsCliResult RunModelsCli(const ModelsCliOptions& options,
                             const ModelsCliRunOptions& run_options);

}  // namespace azookey::host
