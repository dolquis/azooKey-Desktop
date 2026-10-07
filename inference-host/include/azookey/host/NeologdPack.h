#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "azookey/host/HttpDownloader.h"

namespace azookey::host {

// The opt-in neologd_lexicon download pack (auto-word-registration-spec section 15.14).
struct NeologdPackManifest {
  std::string upstream_revision;
  std::string file_name;
  uint64_t size{0};
  std::string sha256;
  std::string url;
  // attribution.sources, re-serialized so it compares equal to the pack's META.sources.
  std::string attribution_sources;

  // Empty url and sha256 mean no pack is published (missing-pack).
  bool published() const { return !url.empty(); }
};

struct NeologdPackManifestParseResult {
  std::optional<NeologdPackManifest> manifest;
  std::string error;
};

// Applies the same rules as dictbuild/neologd_pack.py validate_manifest.
NeologdPackManifestParseResult ParseNeologdPackManifest(std::string_view json);
// dictbuild/packs/neologd_lexicon.manifest.json, embedded at build time.
std::string_view PinnedNeologdPackManifestJson();

class IPackDownloader {
 public:
  virtual ~IPackDownloader() = default;
  // Same contract as HttpDownloader::Download: SHA256 is verified before promotion.
  virtual HttpDownloadResult Download(const HttpDownloadRequest& request) = 0;
};

class HttpPackDownloader final : public IPackDownloader {
 public:
  HttpDownloadResult Download(const HttpDownloadRequest& request) override;

 private:
  HttpDownloader downloader_;
};

enum class NeologdPackStatus : uint8_t { Ready, MissingPack, Failed };

struct NeologdPackResult {
  NeologdPackStatus status{NeologdPackStatus::Failed};
  std::filesystem::path path;
  // Fixed category, safe to log: never a path or a server response.
  std::string error;
};

// Checks (2) and (4)-(6) of section 15.14 on a file whose SHA256 the downloader verified.
// Returns an error category, or nullopt when the pack may be loaded as layer 2.
std::optional<std::string> VerifyNeologdPackFile(const NeologdPackManifest& manifest,
                                                 const std::filesystem::path& path);

// Fetches the published pack into packs_dir, or reuses a valid copy there, then verifies
// it. Other versions of the pack in packs_dir are removed once the current one is ready.
NeologdPackResult PrepareNeologdPack(const NeologdPackManifest& manifest,
                                     const std::filesystem::path& packs_dir,
                                     IPackDownloader& downloader);

}  // namespace azookey::host
