#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/host/ZenzaiPromptFormat.h"

namespace azookey::host {

// Upper limit of a v2 artifact's sizeBytes, so a catalog can never ask for an unbounded
// download. Leaves room for models well beyond the shipped ones (tens of MB).
inline constexpr uint64_t kMaxModelCatalogBytes = uint64_t{4} << 30;  // 4 GiB

// v2 artifacts["llama.cpp"].source: the upstream Hugging Face location of the GGUF.
struct ModelCatalogSource {
  std::string hugging_face_repo;
  // Full 40-hex commit id, never a branch or tag.
  std::string revision;
};

struct ModelCatalogEntry {
  std::string id;
  std::string display_name;
  // The R1 (llama.cpp) artifact: v1 entry keys, or v2 artifacts["llama.cpp"].
  std::string file_name;
  std::string sha256;
  std::string backend_hint{"llama.cpp"};
  // `promptFormat`; an entry without the key is zenz-v3.
  ZenzaiPromptFormat prompt_format{ZenzaiPromptFormat::ZenzV3};
  // v2 only. 0 when absent; a source always comes with a size.
  uint64_t size_bytes{0};
  std::optional<ModelCatalogSource> source;
};

struct ModelCatalog {
  uint32_t schema_version{1};
  std::string default_model_id;
  std::vector<ModelCatalogEntry> models;
};

struct ModelCatalogParseResult {
  bool ok{false};
  ModelCatalog catalog;
  std::optional<std::string> error;
};

enum class ModelCatalogResolveStatus {
  Resolved,
  EmptyCatalog,
  UnknownModelId,
  MissingLocalFile,
};

struct ResolvedModelCatalogEntry {
  ModelCatalogEntry entry;
  std::filesystem::path path;
  bool exists{false};
};

struct ModelCatalogResolveResult {
  ModelCatalogResolveStatus status{ModelCatalogResolveStatus::EmptyCatalog};
  std::optional<ResolvedModelCatalogEntry> model;
  std::optional<std::string> error;

  bool ok() const { return status == ModelCatalogResolveStatus::Resolved; }
};

// Reads schemaVersion 1 and 2 (docs/model-management-spec.md section 3.1.1). A v1 entry
// means the same as a v2 entry whose artifacts["llama.cpp"] holds its fileName and sha256.
ModelCatalogParseResult ParseModelCatalogJson(std::string_view text);
// settings/default-model-catalog.json, embedded at build time.
std::string_view DefaultModelCatalogJson();
// https://huggingface.co/<repo>/resolve/<revision>/<fileName>, with the file name
// percent-encoded. nullopt when the entry has no source.
std::optional<std::string> ModelCatalogDownloadUrl(const ModelCatalogEntry& entry);
// docs/copilot-pc-backend-spec.md section 4.4: the entry whose GGUF model.selectedPath names,
// when the file is directly in zenzai_dir and both its name (without case) and its SHA-256
// match. The caller computes selected_sha256 (the spec keeps it across loads); nullptr when
// nothing matches.
const ModelCatalogEntry* MatchSelectedPathToCatalog(const ModelCatalog& catalog,
                                                    const std::filesystem::path& zenzai_dir,
                                                    const std::filesystem::path& selected_path,
                                                    std::string_view selected_sha256);
std::filesystem::path DefaultZenzaiModelDirectory(const std::filesystem::path& models_dir);
ModelCatalogResolveResult ResolveModelCatalogModel(const ModelCatalog& catalog,
                                                   const std::filesystem::path& zenzai_model_dir,
                                                   std::string_view requested_model_id = {});

}  // namespace azookey::host
