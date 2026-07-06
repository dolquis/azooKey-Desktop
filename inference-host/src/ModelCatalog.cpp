#include "azookey/host/ModelCatalog.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <system_error>

#include "azookey/ipc/Json.h"

namespace azookey::host {

namespace {

namespace j = ::azookey::ipc::json;

ModelCatalogParseResult ParseError(std::string error) {
  ModelCatalogParseResult result;
  result.error = std::move(error);
  return result;
}

const j::Value* FindValue(const j::Object& object, std::string_view key) {
  const auto it = object.find(std::string(key));
  if (it == object.end()) return nullptr;
  return &it->second;
}

bool HasOnlyKeys(const j::Object& object, std::initializer_list<std::string_view> allowed) {
  for (const auto& [key, _] : object) {
    const auto found = std::find(allowed.begin(), allowed.end(), key);
    if (found == allowed.end()) return false;
  }
  return true;
}

std::optional<std::string> RequiredString(const j::Object& object, std::string_view key,
                                          std::string* error) {
  const auto* value = FindValue(object, key);
  if (!value || !value->IsString() || value->AsString().empty()) {
    *error = "model catalog requires non-empty string field: " + std::string(key);
    return std::nullopt;
  }
  return value->AsString();
}

std::optional<std::string> OptionalString(const j::Object& object, std::string_view key,
                                          std::string fallback, std::string* error) {
  const auto* value = FindValue(object, key);
  if (!value) return fallback;
  if (!value->IsString()) {
    *error = "model catalog field must be a string: " + std::string(key);
    return std::nullopt;
  }
  return value->AsString();
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool IsValidIdentifier(std::string_view value) {
  if (value.empty()) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-';
  });
}

bool IsValidSha256(std::string_view value) {
  if (value.size() != 64) return false;
  return std::all_of(value.begin(), value.end(),
                     [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

bool EndsWithGgufExtension(std::string_view value) {
  constexpr std::string_view kExtension = ".gguf";
  if (value.size() <= kExtension.size()) return false;
  const auto tail = value.substr(value.size() - kExtension.size());
  for (size_t i = 0; i < kExtension.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(tail[i])) != kExtension[i]) return false;
  }
  return true;
}

bool HasControlCharacter(std::string_view value) {
  return std::any_of(value.begin(), value.end(),
                     [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}

bool IsSafeModelFileName(std::string_view value) {
  if (value.empty() || value == "." || value == "..") return false;
  if (!EndsWithGgufExtension(value)) return false;
  if (HasControlCharacter(value)) return false;
  return value.find('/') == std::string_view::npos && value.find('\\') == std::string_view::npos &&
         value.find(':') == std::string_view::npos;
}

bool IsAllowedBackendHint(std::string_view value) { return value == "llama.cpp"; }

std::optional<ModelCatalogEntry> ParseModelEntry(const j::Object& object, std::string* error) {
  if (!HasOnlyKeys(object, {"id", "displayName", "fileName", "sha256", "backendHint"})) {
    *error = "model catalog entry contains an unknown field";
    return std::nullopt;
  }

  ModelCatalogEntry entry;
  auto id = RequiredString(object, "id", error);
  if (!id) return std::nullopt;
  if (!IsValidIdentifier(*id)) {
    *error = "model catalog entry id contains unsupported characters: " + *id;
    return std::nullopt;
  }
  entry.id = std::move(*id);

  auto display_name = OptionalString(object, "displayName", entry.id, error);
  if (!display_name) return std::nullopt;
  if (display_name->empty()) {
    *error = "model catalog entry displayName must be non-empty when present";
    return std::nullopt;
  }
  entry.display_name = std::move(*display_name);

  auto file_name = RequiredString(object, "fileName", error);
  if (!file_name) return std::nullopt;
  if (!IsSafeModelFileName(*file_name)) {
    *error = "model catalog entry fileName must be a safe GGUF file name: " + *file_name;
    return std::nullopt;
  }
  entry.file_name = std::move(*file_name);

  auto sha256 = RequiredString(object, "sha256", error);
  if (!sha256) return std::nullopt;
  if (!IsValidSha256(*sha256)) {
    *error = "model catalog entry sha256 must be 64 hex characters";
    return std::nullopt;
  }
  entry.sha256 = LowerAscii(std::move(*sha256));

  auto backend_hint = OptionalString(object, "backendHint", entry.backend_hint, error);
  if (!backend_hint) return std::nullopt;
  if (!IsAllowedBackendHint(*backend_hint)) {
    *error = "model catalog entry backendHint is not supported: " + *backend_hint;
    return std::nullopt;
  }
  entry.backend_hint = std::move(*backend_hint);

  return entry;
}

std::optional<std::string> ReadDefaultModelId(const j::Object& root, std::string* error) {
  const auto* default_id = FindValue(root, "defaultModelId");
  if (!default_id) return std::nullopt;
  if (!default_id->IsString() || default_id->AsString().empty()) {
    *error = "model catalog defaultModelId must be a non-empty string";
    return std::nullopt;
  }
  if (!IsValidIdentifier(default_id->AsString())) {
    *error = "model catalog defaultModelId contains unsupported characters";
    return std::nullopt;
  }
  return default_id->AsString();
}

const ModelCatalogEntry* FindModelById(const ModelCatalog& catalog, std::string_view id) {
  const auto it = std::find_if(catalog.models.begin(), catalog.models.end(),
                               [id](const ModelCatalogEntry& entry) { return entry.id == id; });
  if (it == catalog.models.end()) return nullptr;
  return &*it;
}

}  // namespace

ModelCatalogParseResult ParseModelCatalogJson(std::string_view text) {
  auto parsed = j::Parse(text);
  if (!parsed || !parsed->IsObject()) {
    return ParseError("invalid model catalog JSON");
  }

  const auto& root = parsed->AsObject();
  if (!HasOnlyKeys(root, {"schemaVersion", "defaultModelId", "models"})) {
    return ParseError("model catalog contains an unknown top-level field");
  }

  const auto* schema_version = FindValue(root, "schemaVersion");
  if (!schema_version || !schema_version->IsNumber() || schema_version->AsNumber() != 1.0) {
    return ParseError("model catalog schemaVersion must be 1");
  }

  const auto* models = FindValue(root, "models");
  if (!models || !models->IsArray() || models->AsArray().empty()) {
    return ParseError("model catalog requires a non-empty models array");
  }

  ModelCatalog catalog;
  catalog.schema_version = 1;
  std::set<std::string> seen_ids;
  for (const auto& item : models->AsArray()) {
    if (!item.IsObject()) {
      return ParseError("model catalog models items must be objects");
    }
    std::string error;
    auto entry = ParseModelEntry(item.AsObject(), &error);
    if (!entry) return ParseError(std::move(error));
    if (!seen_ids.insert(entry->id).second) {
      return ParseError("model catalog contains duplicate model id: " + entry->id);
    }
    catalog.models.push_back(std::move(*entry));
  }

  std::string error;
  auto default_model_id = ReadDefaultModelId(root, &error);
  if (!error.empty()) return ParseError(std::move(error));
  catalog.default_model_id = default_model_id.value_or(catalog.models.front().id);
  if (!FindModelById(catalog, catalog.default_model_id)) {
    return ParseError("model catalog defaultModelId does not match a model id: " +
                      catalog.default_model_id);
  }

  ModelCatalogParseResult result;
  result.ok = true;
  result.catalog = std::move(catalog);
  return result;
}

std::filesystem::path DefaultZenzaiModelDirectory(const std::filesystem::path& models_dir) {
  return (models_dir / "zenzai").lexically_normal();
}

ModelCatalogResolveResult ResolveModelCatalogModel(const ModelCatalog& catalog,
                                                   const std::filesystem::path& zenzai_model_dir,
                                                   std::string_view requested_model_id) {
  if (catalog.models.empty() || catalog.default_model_id.empty()) {
    return {ModelCatalogResolveStatus::EmptyCatalog, std::nullopt,
            std::string("model catalog is empty")};
  }

  const auto selected_id =
      requested_model_id.empty() ? std::string_view(catalog.default_model_id) : requested_model_id;
  const auto* entry = FindModelById(catalog, selected_id);
  if (!entry) {
    return {ModelCatalogResolveStatus::UnknownModelId, std::nullopt,
            "model catalog does not contain requested model id: " + std::string(selected_id)};
  }

  ResolvedModelCatalogEntry resolved;
  resolved.entry = *entry;
  resolved.path = (zenzai_model_dir / entry->file_name).lexically_normal();
  std::error_code ec;
  resolved.exists = std::filesystem::is_regular_file(resolved.path, ec) && !ec;
  if (!resolved.exists) {
    const auto message = "model catalog file is not present: " + resolved.path.string();
    return {ModelCatalogResolveStatus::MissingLocalFile, std::move(resolved), message};
  }

  return {ModelCatalogResolveStatus::Resolved, std::move(resolved), std::nullopt};
}

}  // namespace azookey::host
