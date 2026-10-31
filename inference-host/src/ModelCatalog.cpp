#include "azookey/host/ModelCatalog.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <system_error>

#include "azookey/ipc/Json.h"

namespace azookey::host {

namespace {

namespace j = ::azookey::ipc::json;

ModelCatalogParseResult CatalogParseError(std::string error) {
  ModelCatalogParseResult result;
  result.error = std::move(error);
  return result;
}

const j::Value* FindValue(const j::Object& object, std::string_view key) {
  const auto it = object.find(std::string(key));
  if (it == object.end()) return nullptr;
  return &it->second;
}

// The first key outside allowed, so an error can name the field a reader does not know.
std::optional<std::string> FindUnknownKey(const j::Object& object,
                                          std::initializer_list<std::string_view> allowed) {
  for (const auto& [key, _] : object) {
    const auto found = std::find(allowed.begin(), allowed.end(), key);
    if (found == allowed.end()) return key;
  }
  return std::nullopt;
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

// fileName and sha256 of a v1 entry or of a v2 artifacts["llama.cpp"].
bool ParseGgufArtifact(const j::Object& object, ModelCatalogEntry& entry, std::string* error) {
  auto file_name = RequiredString(object, "fileName", error);
  if (!file_name) return false;
  if (!IsSafeModelFileName(*file_name)) {
    *error = "model catalog entry fileName must be a safe GGUF file name: " + *file_name;
    return false;
  }
  entry.file_name = std::move(*file_name);

  auto sha256 = RequiredString(object, "sha256", error);
  if (!sha256) return false;
  if (!IsValidSha256(*sha256)) {
    *error = "model catalog entry sha256 must be 64 hex characters";
    return false;
  }
  entry.sha256 = LowerAscii(std::move(*sha256));
  return true;
}

bool IsHuggingFaceName(std::string_view value) {
  if (value.empty() || !std::isalnum(static_cast<unsigned char>(value.front()))) return false;
  return IsValidIdentifier(value) && value.find("..") == std::string_view::npos;
}

// owner/name, each part [A-Za-z0-9][A-Za-z0-9._-]* without "..".
bool IsValidHuggingFaceRepo(std::string_view value) {
  const auto slash = value.find('/');
  if (slash == std::string_view::npos) return false;
  return IsHuggingFaceName(value.substr(0, slash)) && IsHuggingFaceName(value.substr(slash + 1));
}

bool IsFullCommitId(std::string_view value) {
  if (value.size() != 40) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
  });
}

std::optional<ModelCatalogSource> ParseSource(const j::Value& value, std::string* error) {
  if (!value.IsObject()) {
    *error = "model catalog artifact source must be an object";
    return std::nullopt;
  }
  const auto& object = value.AsObject();
  if (const auto unknown = FindUnknownKey(object, {"huggingFaceRepo", "revision"})) {
    *error = "model catalog artifact source contains an unknown field: " + *unknown;
    return std::nullopt;
  }
  ModelCatalogSource source;
  auto repo = RequiredString(object, "huggingFaceRepo", error);
  if (!repo) return std::nullopt;
  if (!IsValidHuggingFaceRepo(*repo)) {
    *error = "model catalog artifact source huggingFaceRepo must be owner/name: " + *repo;
    return std::nullopt;
  }
  source.hugging_face_repo = std::move(*repo);
  auto revision = RequiredString(object, "revision", error);
  if (!revision) return std::nullopt;
  if (!IsFullCommitId(*revision)) {
    *error = "model catalog artifact source revision must be a 40-character lowercase commit id";
    return std::nullopt;
  }
  source.revision = std::move(*revision);
  return source;
}

// JSON numbers are doubles; 2^53 - 1 keeps every accepted size exact.
constexpr double kMaxExactSizeBytes = 9007199254740991.0;

bool ParseLlamaCppArtifact(const j::Value& value, ModelCatalogEntry& entry, std::string* error) {
  if (!value.IsObject()) {
    *error = "model catalog artifact llama.cpp must be an object";
    return false;
  }
  const auto& object = value.AsObject();
  if (const auto unknown = FindUnknownKey(object, {"fileName", "sha256", "sizeBytes", "source"})) {
    *error = "model catalog artifact llama.cpp contains an unknown field: " + *unknown;
    return false;
  }
  if (!ParseGgufArtifact(object, entry, error)) return false;

  if (const auto* size = FindValue(object, "sizeBytes")) {
    const double bytes = size->IsNumber() ? size->AsNumber() : 0.0;
    if (!(bytes >= 1.0 && bytes <= kMaxExactSizeBytes) || bytes != std::floor(bytes)) {
      *error = "model catalog artifact sizeBytes must be a positive integer";
      return false;
    }
    entry.size_bytes = static_cast<uint64_t>(bytes);
  }
  if (const auto* source = FindValue(object, "source")) {
    auto parsed = ParseSource(*source, error);
    if (!parsed) return false;
    if (entry.size_bytes == 0) {
      *error = "model catalog artifact with a source requires sizeBytes";
      return false;
    }
    entry.source = std::move(*parsed);
  }
  return true;
}

bool ParseArtifacts(const j::Value& value, ModelCatalogEntry& entry, std::string* error) {
  if (!value.IsObject()) {
    *error = "model catalog entry artifacts must be an object";
    return false;
  }
  const auto& object = value.AsObject();
  // onnxruntime-genai (R2) stays undefined until DEV-1768 settles the artifact's contents.
  if (const auto unknown = FindUnknownKey(object, {"llama.cpp"})) {
    *error = "model catalog artifacts engine is not supported: " + *unknown;
    return false;
  }
  const auto* llama = FindValue(object, "llama.cpp");
  if (!llama) {
    *error = "model catalog entry artifacts requires llama.cpp";
    return false;
  }
  return ParseLlamaCppArtifact(*llama, entry, error);
}

std::optional<ModelCatalogEntry> ParseModelEntry(const j::Object& object, uint32_t version,
                                                 std::string* error) {
  const auto unknown =
      version == 1 ? FindUnknownKey(object, {"id", "displayName", "fileName", "sha256",
                                             "backendHint", "promptFormat"})
                   : FindUnknownKey(object, {"id", "displayName", "promptFormat", "artifacts"});
  if (unknown) {
    *error = "model catalog entry contains an unknown field for schemaVersion " +
             std::to_string(version) + ": " + *unknown;
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

  if (version == 1) {
    if (!ParseGgufArtifact(object, entry, error)) return std::nullopt;
    auto backend_hint = OptionalString(object, "backendHint", entry.backend_hint, error);
    if (!backend_hint) return std::nullopt;
    if (!IsAllowedBackendHint(*backend_hint)) {
      *error = "model catalog entry backendHint is not supported: " + *backend_hint;
      return std::nullopt;
    }
    entry.backend_hint = std::move(*backend_hint);
  } else {
    const auto* artifacts = FindValue(object, "artifacts");
    if (!artifacts) {
      *error = "model catalog entry requires artifacts";
      return std::nullopt;
    }
    if (!ParseArtifacts(*artifacts, entry, error)) return std::nullopt;
  }

  auto prompt_format_name = OptionalString(
      object, "promptFormat", std::string(ZenzaiPromptFormatName(entry.prompt_format)), error);
  if (!prompt_format_name) return std::nullopt;
  const auto prompt_format = ParseZenzaiPromptFormat(*prompt_format_name);
  if (!prompt_format) {
    *error = "model catalog entry promptFormat is not supported: " + *prompt_format_name;
    return std::nullopt;
  }
  entry.prompt_format = *prompt_format;

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
    return CatalogParseError("invalid model catalog JSON");
  }

  const auto& root = parsed->AsObject();
  if (const auto unknown = FindUnknownKey(root, {"schemaVersion", "defaultModelId", "models"})) {
    return CatalogParseError("model catalog contains an unknown top-level field: " + *unknown);
  }

  const auto* schema_version = FindValue(root, "schemaVersion");
  if (!schema_version || !schema_version->IsNumber() ||
      (schema_version->AsNumber() != 1.0 && schema_version->AsNumber() != 2.0)) {
    return CatalogParseError("model catalog schemaVersion must be 1 or 2");
  }
  const uint32_t version = schema_version->AsNumber() == 1.0 ? 1 : 2;

  const auto* models = FindValue(root, "models");
  if (!models || !models->IsArray() || models->AsArray().empty()) {
    return CatalogParseError("model catalog requires a non-empty models array");
  }

  ModelCatalog catalog;
  catalog.schema_version = version;
  std::set<std::string> seen_ids;
  for (const auto& item : models->AsArray()) {
    if (!item.IsObject()) {
      return CatalogParseError("model catalog models items must be objects");
    }
    std::string error;
    auto entry = ParseModelEntry(item.AsObject(), version, &error);
    if (!entry) return CatalogParseError(std::move(error));
    if (!seen_ids.insert(entry->id).second) {
      return CatalogParseError("model catalog contains duplicate model id: " + entry->id);
    }
    catalog.models.push_back(std::move(*entry));
  }

  std::string error;
  auto default_model_id = ReadDefaultModelId(root, &error);
  if (!error.empty()) return CatalogParseError(std::move(error));
  catalog.default_model_id = default_model_id.value_or(catalog.models.front().id);
  if (!FindModelById(catalog, catalog.default_model_id)) {
    return CatalogParseError("model catalog defaultModelId does not match a model id: " +
                             catalog.default_model_id);
  }

  ModelCatalogParseResult result;
  result.ok = true;
  result.catalog = std::move(catalog);
  return result;
}

std::optional<std::string> ModelCatalogDownloadUrl(const ModelCatalogEntry& entry) {
  if (!entry.source) return std::nullopt;
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string url = "https://huggingface.co/" + entry.source->hugging_face_repo + "/resolve/" +
                    entry.source->revision + "/";
  for (const unsigned char ch : entry.file_name) {
    if (std::isalnum(ch) || ch == '-' || ch == '.' || ch == '_' || ch == '~') {
      url.push_back(static_cast<char>(ch));
    } else {
      url.push_back('%');
      url.push_back(kHex[ch >> 4]);
      url.push_back(kHex[ch & 0x0f]);
    }
  }
  return url;
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
