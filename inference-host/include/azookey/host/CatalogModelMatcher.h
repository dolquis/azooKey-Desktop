#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "azookey/host/ModelCatalog.h"
#include "azookey/host/ZenzaiPromptFormat.h"

namespace azookey::host {

// model.selectedPath as MatchSelectedPathToCatalog compares it: without a \\?\ prefix and
// without trailing separators. 8.3 short names are left as they are (they do not match).
std::filesystem::path NormalizeSelectedModelPath(std::string_view selected_path_utf8);

using FileSha256Function =
    std::function<std::optional<std::string>(const std::filesystem::path& path)>;

enum class CatalogEntryFileState : uint8_t {
  Missing,   // No file of the entry's name in zenzai_dir.
  Verified,  // The file's SHA-256 is the entry's.
  Mismatch,  // A file of that name with another SHA-256, or one that could not be read.
};

std::string_view CatalogEntryFileStateName(CatalogEntryFileState state);

// The sidecar in zenzai_dir that keeps the matcher's SHA-256 across Host restarts (DEV-1841):
// {"schemaVersion":1,"entries":[{"path":<file name>,"size":N,"mtime":<write time ticks>,
// "sha256":"..."}]}.
inline constexpr std::string_view kCatalogSha256SidecarName = ".catalog-sha256.json";

// docs/copilot-pc-backend-spec.md section 4.4: matches model.selectedPath against a catalog.
// The SHA-256 of each file is kept for the file's size and last write time and computed again
// only when either changes. The kept values of files directly in zenzai_dir are also written to
// the sidecar and read back by the next process; a sidecar that cannot be read or parsed, or
// of another schemaVersion, is ignored and rewritten. A file whose name matches no entry is
// never hashed. Thread-safe.
class CatalogModelMatcher {
 public:
  // sha256 defaults to ComputeFileSha256 (Windows only; elsewhere nothing matches).
  CatalogModelMatcher(ModelCatalog catalog, std::filesystem::path zenzai_dir,
                      FileSha256Function sha256 = {});

  const ModelCatalog& catalog() const { return catalog_; }
  const std::filesystem::path& zenzai_dir() const { return zenzai_dir_; }

  // The entry model.selectedPath names, when the file is directly in zenzai_dir and both its
  // name (without case) and its SHA-256 match.
  std::optional<ModelCatalogEntry> Match(std::string_view selected_path_utf8);
  // The matched entry's promptFormat; nullopt leaves the format to the GGUF vocabulary.
  std::optional<ZenzaiPromptFormat> PromptFormatFor(std::string_view selected_path_utf8);
  CatalogEntryFileState StateOf(const ModelCatalogEntry& entry);

 private:
  struct CachedHash {
    uintmax_t size{0};
    std::filesystem::file_time_type write_time;
    std::string sha256;
  };

  std::optional<std::string> Sha256Of(const std::filesystem::path& path);
  // Reads the sidecar once, on the first hash asked for; mutex_ is held.
  void LoadSidecarLocked();
  // Writes the kept hashes of zenzai_dir's files, merged with what the sidecar holds now (another
  // process may have written it), keeping only files still of that size and write time.
  void SaveSidecar();

  const ModelCatalog catalog_;
  const std::filesystem::path zenzai_dir_;
  FileSha256Function sha256_;
  std::mutex mutex_;
  std::map<std::filesystem::path, CachedHash> hashes_;
  bool sidecar_loaded_{false};
};

// For ModelLoadOptions::resolve_prompt_format: the promptFormat of the entry model_path
// matches, asked on the loading thread. Empty when matcher is null or model_path is empty; a
// failure to match (an exception included) gives nullopt.
std::function<std::optional<ZenzaiPromptFormat>()> CatalogPromptFormatResolver(
    std::shared_ptr<CatalogModelMatcher> matcher, std::string model_path);

// The embedded default catalog (DefaultModelCatalogJson) over
// DefaultZenzaiModelDirectory(models_dir). nullptr when models_dir is empty or the catalog
// does not parse.
std::shared_ptr<CatalogModelMatcher> MakeDefaultCatalogModelMatcher(
    const std::filesystem::path& models_dir, FileSha256Function sha256 = {});

}  // namespace azookey::host
