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
enum class ModelsCliCommand { List, Bench };

struct ModelsCliOptions {
  ModelsCliCommand command{ModelsCliCommand::List};
  bool json{false};
  ipc::ListModelsRequest list;
  ipc::BenchmarkModelRequest bench;
};

struct ModelsCliRunOptions {
  std::filesystem::path models_dir;
};

struct ModelsCliResult {
  int exit_code{0};
  std::vector<std::string> output_lines;
  std::string error;
};

// models list [--dir <path>] [--sha256] [--json]
// models bench --path <gguf> [--backend cpu|cuda|vulkan] [--iterations N]
//              [--warmup N] [--case <reading>]... [--json]
std::optional<ModelsCliOptions> ParseModelsCliArgs(const std::vector<std::string>& args,
                                                   std::string* error);
ModelsCliResult RunModelsCli(const ModelsCliOptions& options,
                             const ModelsCliRunOptions& run_options);

}  // namespace azookey::host
