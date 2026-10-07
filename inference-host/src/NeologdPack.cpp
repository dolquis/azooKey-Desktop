#include "azookey/host/NeologdPack.h"

#include <algorithm>
#include <set>
#include <system_error>

#include "azookey/core/DoubleArrayTrie.h"
#include "azookey/ipc/Json.h"

namespace azookey::host {

namespace {

namespace j = ::azookey::ipc::json;

constexpr std::string_view kPackId = "neologd_lexicon";
constexpr uint64_t kLayerId = 2;
constexpr std::string_view kBuilderVersion = "azdic-1";

NeologdPackManifestParseResult ParseError(std::string error) {
  NeologdPackManifestParseResult result;
  result.error = std::move(error);
  return result;
}

bool IsRevision(std::string_view value) {
  if (value.empty() || value.size() > 64) return false;
  const auto alnum = [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  };
  if (!alnum(static_cast<unsigned char>(value.front()))) return false;
  return std::all_of(value.begin(), value.end(),
                     [&](unsigned char c) { return alnum(c) || c == '.' || c == '_' || c == '-'; });
}

bool IsLowerSha256(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

// The URL is handed to WinHTTP as UTF-16; printable ASCII keeps the widening exact.
bool IsPrintableAscii(std::string_view value) {
  return std::all_of(value.begin(), value.end(),
                     [](unsigned char c) { return c > 0x20 && c < 0x7f; });
}

std::string PackFileName(std::string_view revision) {
  return std::string(kPackId) + "-" + std::string(revision) + ".azdic";
}

// Python's validate_manifest accepts only JSON integers, so "1.0" and "1e0" are rejected here
// too even though GetUInt would read them.
std::optional<uint64_t> PlainUInt(const j::Value& object, std::string_view key) {
  const auto* value = object.Find(key);
  if (!value || !value->IsNumber()) return std::nullopt;
  const auto& token = value->AsNumberValue().token;
  if (token.empty() || !std::all_of(token.begin(), token.end(),
                                    [](unsigned char c) { return c >= '0' && c <= '9'; }))
    return std::nullopt;
  return object.GetUInt(key);
}

bool UIntEquals(const j::Value& object, std::string_view key, uint64_t expected) {
  const auto value = PlainUInt(object, key);
  return value && *value == expected;
}

bool StartsWith(const std::filesystem::path::string_type& text,
                const std::filesystem::path::string_type& prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::filesystem::path::string_type& text,
              const std::filesystem::path::string_type& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Old versions are no longer referenced. Removal is best effort: a leftover file only costs
// disk space and is retried at the next start, so no error here may fail a ready pack.
void RemoveOtherPackVersions(const std::filesystem::path& packs_dir, const std::string& keep) {
  const auto prefix = std::filesystem::path(std::string(kPackId) + "-").native();
  const auto pack = std::filesystem::path(".azdic").native();
  const auto part = std::filesystem::path(".azdic.part").native();
  const auto kept = std::filesystem::path(keep).native();
  const auto kept_part = std::filesystem::path(keep + ".part").native();
  std::error_code ec;
  std::filesystem::directory_iterator it(packs_dir, ec);
  for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    const auto name = it->path().filename().native();
    if (!StartsWith(name, prefix) || !(EndsWith(name, pack) || EndsWith(name, part)) ||
        name == kept || name == kept_part)
      continue;
    std::error_code remove_error;
    std::filesystem::remove(it->path(), remove_error);
  }
}

}  // namespace

NeologdPackManifestParseResult ParseNeologdPackManifest(std::string_view json) {
  const auto parsed = j::Parse(json);
  if (!parsed || !parsed->IsObject()) return ParseError("manifest is not a JSON object");
  const auto& root = *parsed;
  // Not static: the startup worker may still run while static storage is torn down.
  const std::set<std::string> kFields = {"manifest_version",
                                         "pack_id",
                                         "layer_id",
                                         "format",
                                         "format_version",
                                         "builder_version",
                                         "upstream_revision",
                                         "file_name",
                                         "size",
                                         "sha256",
                                         "url",
                                         "attribution"};
  std::set<std::string> fields;
  for (const auto& [key, _] : root.AsObject()) fields.insert(key);
  if (fields != kFields) return ParseError("manifest fields do not match version 1");
  if (!UIntEquals(root, "manifest_version", 1) || root.GetString("pack_id") != kPackId ||
      !UIntEquals(root, "layer_id", kLayerId) || root.GetString("format") != "azdic" ||
      !UIntEquals(root, "format_version", 1) ||
      root.GetString("builder_version") != kBuilderVersion)
    return ParseError("unsupported manifest");

  NeologdPackManifest manifest;
  for (const auto& [key, out] :
       {std::pair{"upstream_revision", &manifest.upstream_revision},
        std::pair{"file_name", &manifest.file_name}, std::pair{"sha256", &manifest.sha256},
        std::pair{"url", &manifest.url}}) {
    const auto value = root.GetString(key);
    if (!value) return ParseError("manifest string field has the wrong type");
    *out = *value;
  }
  const auto size = PlainUInt(root, "size");
  if (!size) return ParseError("manifest size must be a non-negative integer");
  manifest.size = *size;
  const auto* attribution = root.Find("attribution");
  const auto* sources = attribution ? attribution->GetArray("sources") : nullptr;
  if (!sources || sources->empty() || !attribution->GetString("notices"))
    return ParseError("manifest attribution is missing");
  manifest.attribution_sources = j::Stringify(j::Value(*sources));

  if (manifest.url.empty() && manifest.sha256.empty()) {
    if (!manifest.upstream_revision.empty() || !manifest.file_name.empty() || manifest.size)
      return ParseError("unpublished manifest must not name a pack");
    return {manifest, {}};
  }
  if (manifest.url.rfind("https://", 0) != 0 || !IsPrintableAscii(manifest.url))
    return ParseError("pack url must be https");
  if (!IsLowerSha256(manifest.sha256))
    return ParseError("pack sha256 must be 64 lowercase hex digits");
  if (!manifest.size || !IsRevision(manifest.upstream_revision) ||
      manifest.file_name != PackFileName(manifest.upstream_revision))
    return ParseError("published manifest must name a nonempty, versioned pack");
  return {manifest, {}};
}

HttpDownloadResult HttpPackDownloader::Download(const HttpDownloadRequest& request) {
  return downloader_.Download(request);
}

std::optional<std::string> VerifyNeologdPackFile(const NeologdPackManifest& manifest,
                                                 const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size != manifest.size) return "pack size mismatch";
  core::DoubleArrayTrie trie;
  if (!trie.Load(path, true)) return "invalid pack artifact";
  if (trie.LayerId() != kLayerId) return "pack layer id mismatch";
  const auto meta = j::Parse(trie.Metadata());
  const auto* sources = meta && meta->IsObject() ? meta->GetArray("sources") : nullptr;
  if (!sources || j::Stringify(j::Value(*sources)) != manifest.attribution_sources)
    return "pack attribution mismatch";
  if (meta->GetString("builder_version") != kBuilderVersion) return "pack builder version mismatch";
  return std::nullopt;
}

NeologdPackResult PrepareNeologdPack(const NeologdPackManifest& manifest,
                                     const std::filesystem::path& packs_dir,
                                     IPackDownloader& downloader) {
  NeologdPackResult result;
  if (!manifest.published()) {
    result.status = NeologdPackStatus::MissingPack;
    return result;
  }
  std::error_code ec;
  std::filesystem::create_directories(packs_dir, ec);
  if (ec) {
    result.error = "packs directory unavailable";
    return result;
  }
  HttpDownloadRequest request;
  request.url.assign(manifest.url.begin(), manifest.url.end());
  request.destination = packs_dir / manifest.file_name;
  request.expected_sha256 = manifest.sha256;
  request.max_bytes = manifest.size;
  // The downloader reuses a destination whose SHA256 already matches, without network.
  if (!downloader.Download(request).ok()) {
    result.error = "pack download failed";
    return result;
  }
  if (auto error = VerifyNeologdPackFile(manifest, request.destination)) {
    result.error = std::move(*error);
    return result;
  }
  RemoveOtherPackVersions(packs_dir, manifest.file_name);
  result.status = NeologdPackStatus::Ready;
  result.path = request.destination;
  return result;
}

}  // namespace azookey::host
