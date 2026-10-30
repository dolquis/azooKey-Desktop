#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/host/ZenzaiPromptFormat.h"

namespace azookey::host {

struct ModelCatalogEntry {
  std::string id;
  std::string display_name;
  std::string file_name;
  std::string sha256;
  std::string backend_hint{"llama.cpp"};
  // `promptFormat`; an entry without the key is zenz-v3.
  ZenzaiPromptFormat prompt_format{ZenzaiPromptFormat::ZenzV3};
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

ModelCatalogParseResult ParseModelCatalogJson(std::string_view text);
std::filesystem::path DefaultZenzaiModelDirectory(const std::filesystem::path& models_dir);
ModelCatalogResolveResult ResolveModelCatalogModel(const ModelCatalog& catalog,
                                                   const std::filesystem::path& zenzai_model_dir,
                                                   std::string_view requested_model_id = {});

}  // namespace azookey::host
