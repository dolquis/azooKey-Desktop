#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/ipc/Payloads.h"

namespace azookey::host {

// docs/model-management-spec.md section 3. A model found on disk; the name
// differs from the DEV-438 declarative ModelCatalogEntry on purpose.
enum class LocalModelFormat : uint8_t {
  Gguf,       // R1: a *.gguf file.
  OnnxGenAi,  // R2: a directory holding genai_config.json (detected only, not run).
};

struct LocalModelEntry {
  std::filesystem::path path;
  std::string file_name;
  LocalModelFormat format{LocalModelFormat::Gguf};
  uint64_t size_bytes{};
  bool valid{false};
  // Fixed category when invalid, e.g. "magic_mismatch"; never file contents.
  std::string invalid_reason;
  std::string model_family;
  std::string quantization;
  uint64_t n_params{};
  std::string sha256;
};

struct ModelScanOptions {
  bool compute_sha256{false};
  size_t max_entries{256};
};

// Lists R1 files and R2 directories directly in dir and in its immediate
// subdirectories (section 3.1.2: one level), sorted by path. Unreadable,
// broken or missing entries never throw; a missing dir yields an empty list.
std::vector<LocalModelEntry> ScanModelDirectory(const std::filesystem::path& dir,
                                                const ModelScanOptions& options = {});

// R1: reads at most the first 4 KiB (section 8) and checks the magic, the
// version, the header counts and general.architecture when the key-value
// section fits in that window.
LocalModelEntry InspectGgufFile(const std::filesystem::path& path);

// R2: genai_config.json must parse and every file it references (decoder and
// pipeline stage filenames) must exist, plus tokenizer.json (section 3.3).
LocalModelEntry InspectOnnxGenAiDirectory(const std::filesystem::path& dir);

// The directory a ListModels request may scan. Empty selects models_root;
// otherwise a leading "%LOCALAPPDATA%" is expanded and the result must be
// models_root or inside it. nullopt rejects the request.
std::optional<std::filesystem::path> ResolveModelListingDirectory(
    std::string_view requested, const std::filesystem::path& models_root);

ipc::ListedModel ToListedModel(const LocalModelEntry& entry, std::string last_load_status,
                               std::string last_error);

}  // namespace azookey::host
