#include "azookey/host/CatalogModelMatcher.h"

#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "azookey/core/PlatformPaths.h"
#include "azookey/host/HttpDownloader.h"
#include "azookey/ipc/Json.h"
#include "azookey/learning/AtomicFile.h"

namespace azookey::host {
namespace {

namespace j = ::azookey::ipc::json;

constexpr int64_t kSidecarSchemaVersion = 1;
// The sidecar holds one entry per catalog file; anything larger is not one this Host wrote.
constexpr uintmax_t kMaxSidecarBytes = 1024 * 1024;
constexpr size_t kMaxSidecarEntries = 256;

struct SidecarEntry {
  uintmax_t size{0};
  int64_t mtime{0};
  std::string sha256;
};

// The write time as the sidecar stores it: the clock's own ticks, for this machine only.
int64_t WriteTimeTicks(std::filesystem::file_time_type time) {
  return static_cast<int64_t>(time.time_since_epoch().count());
}

// A bare file name: the sidecar never names a file outside zenzai_dir. A name Win32 would trim
// or read otherwise (a trailing dot or space, a control character, a stream after ':') is
// refused too, so an entry never stands for another file.
bool IsPlainFileName(std::string_view name) {
  if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ') {
    return false;
  }
  for (const char c : name) {
    if (static_cast<unsigned char>(c) < 0x20 || c == '/' || c == '\\' || c == ':') return false;
  }
  return true;
}

bool IsSha256Hex(std::string_view text) {
  if (text.size() != 64) return false;
  for (const char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// The sidecar's entries by file name. A missing, unreadable, oversized or unparsable file, or
// one of another schemaVersion, gives none (the hashes are computed again); a malformed entry
// is skipped.
std::map<std::string, SidecarEntry> ReadSidecar(const std::filesystem::path& file) {
  std::map<std::string, SidecarEntry> out;
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec || size > kMaxSidecarBytes) return out;
  std::ifstream in(file, std::ios::binary);
  if (!in) return out;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) return out;
  const auto root = j::Parse(text);
  if (!root || !root->IsObject() || root->GetInt("schemaVersion") != kSidecarSchemaVersion) {
    return out;
  }
  const auto* entries = root->GetArray("entries");
  if (!entries) return out;
  for (const auto& entry : *entries) {
    if (out.size() >= kMaxSidecarEntries) break;
    if (!entry.IsObject()) continue;
    auto name = entry.GetString("path");
    const auto bytes = entry.GetUInt("size");
    const auto mtime = entry.GetInt("mtime");
    auto sha256 = entry.GetString("sha256");
    if (!name || !bytes || !mtime || !sha256 || !IsPlainFileName(*name) || !IsSha256Hex(*sha256)) {
      continue;
    }
    out[std::move(*name)] = SidecarEntry{*bytes, *mtime, std::move(*sha256)};
  }
  return out;
}

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
    LoadSidecarLocked();
    const auto it = hashes_.find(path);
    if (it != hashes_.end() && it->second.size == size && it->second.write_time == write_time) {
      return it->second.sha256;
    }
  }
  // Hashed outside the lock: a large model takes seconds, and other callers may want another
  // file. Two callers hashing the same file at once both store the same value.
  auto sha256 = sha256_(path);
  if (!sha256) return std::nullopt;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    hashes_[path] = CachedHash{size, write_time, *sha256};
  }
  if (path.parent_path() == ResolvedPath(zenzai_dir_)) SaveSidecar();
  return sha256;
}

void CatalogModelMatcher::LoadSidecarLocked() {
  if (sidecar_loaded_) return;
  sidecar_loaded_ = true;
  const auto zenzai_dir = ResolvedPath(zenzai_dir_);
  for (auto& [name, entry] : ReadSidecar(zenzai_dir / kCatalogSha256SidecarName)) {
    const auto path = (zenzai_dir / core::Utf8Path(name)).lexically_normal();
    // A hash this process computed is newer than the sidecar's.
    hashes_.emplace(path, CachedHash{entry.size,
                                     std::filesystem::file_time_type(
                                         std::filesystem::file_time_type::duration(entry.mtime)),
                                     std::move(entry.sha256)});
  }
}

void CatalogModelMatcher::SaveSidecar() {
  const auto zenzai_dir = ResolvedPath(zenzai_dir_);
  const auto file = zenzai_dir / kCatalogSha256SidecarName;
  // What another process wrote meanwhile stays, unless this process knows newer.
  auto merged = ReadSidecar(file);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [path, hash] : hashes_) {
      if (path.parent_path() != zenzai_dir) continue;
      merged[core::PathToUtf8(path.filename())] =
          SidecarEntry{hash.size, WriteTimeTicks(hash.write_time), hash.sha256};
    }
  }
  j::Array entries;
  for (const auto& [name, entry] : merged) {
    if (entries.size() >= kMaxSidecarEntries) break;
    // Only files still there with that size and write time: a removed or replaced file's
    // entry would never be used again.
    const auto path = zenzai_dir / core::Utf8Path(name);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size != entry.size) continue;
    const auto write_time = std::filesystem::last_write_time(path, ec);
    if (ec || WriteTimeTicks(write_time) != entry.mtime) continue;
    j::Object out;
    out.emplace("path", j::Value(name));
    out.emplace("size", j::Value(static_cast<uint64_t>(entry.size)));
    out.emplace("mtime", j::Value(entry.mtime));
    out.emplace("sha256", j::Value(entry.sha256));
    entries.emplace_back(std::move(out));
  }
  j::Object root;
  root.emplace("schemaVersion", j::Value(kSidecarSchemaVersion));
  root.emplace("entries", j::Value(std::move(entries)));
  // A failure (a read-only folder, for one) only means the next process hashes again.
  learning::WriteTextFileAtomically(file, j::Stringify(j::Value(std::move(root))));
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
