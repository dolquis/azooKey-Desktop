#include "azookey/host/CatalogModelMatcher.h"

#include <system_error>
#include <utility>

#include "azookey/core/PlatformPaths.h"
#include "azookey/host/HttpDownloader.h"

namespace azookey::host {
namespace {

// The long, resolved form of a path: ListModels lists weakly_canonical paths, and a models_dir
// or selectedPath may be spelled with 8.3 short names (a TEMP under RUNNER~1, for one). A path
// that cannot be resolved is used as written.
std::filesystem::path ResolvedPath(const std::filesystem::path& path) {
  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(path, ec);
  return ec ? path : canonical.lexically_normal();
}

}  // namespace

std::filesystem::path NormalizeSelectedModelPath(std::string_view selected_path_utf8) {
  std::string text(selected_path_utf8);
  // \\?\UNC\server\share is \\server\share; \\?\C:\x is C:\x.
  if (text.starts_with(R"(\\?\UNC\)")) {
    text = R"(\\)" + text.substr(8);
  } else if (text.starts_with(R"(\\?\)")) {
    text = text.substr(4);
  }
  while (text.size() > 1 && (text.back() == '\\' || text.back() == '/')) text.pop_back();
  return core::Utf8Path(text).lexically_normal();
}

std::string_view CatalogEntryFileStateName(CatalogEntryFileState state) {
  switch (state) {
    case CatalogEntryFileState::Verified:
      return "verified";
    case CatalogEntryFileState::Mismatch:
      return "mismatch";
    case CatalogEntryFileState::Missing:
      break;
  }
  return "missing";
}

CatalogModelMatcher::CatalogModelMatcher(ModelCatalog catalog, std::filesystem::path zenzai_dir,
                                         FileSha256Function sha256)
    : catalog_(std::move(catalog)),
      zenzai_dir_(zenzai_dir.lexically_normal()),
      sha256_(std::move(sha256)) {
  if (!sha256_) {
    sha256_ = [](const std::filesystem::path& path) { return ComputeFileSha256(path, nullptr); };
  }
}

std::optional<std::string> CatalogModelMatcher::Sha256Of(const std::filesystem::path& file) {
  // Keyed by the resolved form, so a short and a long spelling of one file share the hash.
  const auto path = ResolvedPath(file);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) return std::nullopt;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return std::nullopt;
  const auto write_time = std::filesystem::last_write_time(path, ec);
  if (ec) return std::nullopt;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = hashes_.find(path);
    if (it != hashes_.end() && it->second.size == size && it->second.write_time == write_time) {
      return it->second.sha256;
    }
  }
  // Hashed outside the lock: a large model takes seconds, and other callers may want another
  // file. Two callers hashing the same file at once both store the same value.
  auto sha256 = sha256_(path);
  if (!sha256) return std::nullopt;
  std::lock_guard<std::mutex> lock(mutex_);
  hashes_[path] = CachedHash{size, write_time, *sha256};
  return sha256;
}

std::optional<ModelCatalogEntry> CatalogModelMatcher::Match(std::string_view selected_path_utf8) {
  if (selected_path_utf8.empty()) return std::nullopt;
  // Both sides in the resolved form, so short and long spellings compare equal.
  const auto selected = ResolvedPath(NormalizeSelectedModelPath(selected_path_utf8));
  const auto zenzai_dir = ResolvedPath(zenzai_dir_);
  // Only a file whose place and name match an entry is hashed: passing each entry's own
  // SHA-256 checks everything but the content.
  bool named = false;
  for (const auto& entry : catalog_.models) {
    if (MatchSelectedPathToCatalog(catalog_, zenzai_dir, selected, entry.sha256)) {
      named = true;
      break;
    }
  }
  if (!named) return std::nullopt;
  const auto sha256 = Sha256Of(selected);
  if (!sha256) return std::nullopt;
  const auto* entry = MatchSelectedPathToCatalog(catalog_, zenzai_dir, selected, *sha256);
  if (!entry) return std::nullopt;
  return *entry;
}

std::optional<ZenzaiPromptFormat> CatalogModelMatcher::PromptFormatFor(
    std::string_view selected_path_utf8) {
  const auto entry = Match(selected_path_utf8);
  if (!entry) return std::nullopt;
  return entry->prompt_format;
}

CatalogEntryFileState CatalogModelMatcher::StateOf(const ModelCatalogEntry& entry) {
  const auto path = (zenzai_dir_ / entry.file_name).lexically_normal();
  std::error_code ec;
  if (std::filesystem::symlink_status(path, ec).type() == std::filesystem::file_type::not_found) {
    return CatalogEntryFileState::Missing;
  }
  const auto sha256 = Sha256Of(path);
  return sha256 && *sha256 == entry.sha256 ? CatalogEntryFileState::Verified
                                           : CatalogEntryFileState::Mismatch;
}

std::function<std::optional<ZenzaiPromptFormat>()> CatalogPromptFormatResolver(
    std::shared_ptr<CatalogModelMatcher> matcher, std::string model_path) {
  if (!matcher || model_path.empty()) return {};
  return [matcher = std::move(matcher),
          model_path = std::move(model_path)]() -> std::optional<ZenzaiPromptFormat> {
    try {
      return matcher->PromptFormatFor(model_path);
    } catch (...) {
      // Unmatched: the format comes from the GGUF vocabulary, as without a catalog.
      return std::nullopt;
    }
  };
}

std::shared_ptr<CatalogModelMatcher> MakeDefaultCatalogModelMatcher(
    const std::filesystem::path& models_dir, FileSha256Function sha256) {
  if (models_dir.empty()) return nullptr;
  auto parsed = ParseModelCatalogJson(DefaultModelCatalogJson());
  if (!parsed.ok) return nullptr;
  return std::make_shared<CatalogModelMatcher>(
      std::move(parsed.catalog), DefaultZenzaiModelDirectory(models_dir), std::move(sha256));
}

}  // namespace azookey::host
