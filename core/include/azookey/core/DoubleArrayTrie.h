#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::core {

enum class MatchKind : uint8_t { Exact, Alias, LongVowelRelaxed };

struct PrefixMatch {
  uint32_t key_length{};
  uint32_t key_id{};
  uint32_t entry_ref_off{};
  uint32_t entry_count{};
};

struct StaticDictionaryEntry {
  std::string surface;
  std::string reading;
  uint16_t pos_id{};
  uint16_t category_mask{};
  int16_t cost{};
  double frequency{};
  uint8_t source{};
  MatchKind kind{};
};

// One ENTS record without copied strings (lattice-conversion-spec section 4.2). The surface
// view is range-checked but not UTF-8-checked, and is valid until the trie is loaded again.
struct StaticEntryRef {
  uint32_t index{};
  std::string_view surface;
  uint16_t pos_id{};
  int16_t cost{};
  MatchKind kind{};
};

bool IsValidUtf8(std::string_view text) noexcept;
std::string NormalizeReading(std::string_view text);
std::vector<std::string> ReadingAliases(std::string_view normalized);

// Owns the mapping and invalidates only this dictionary on malformed references.
// The owner serializes access, including queries (a query can invalidate it).
class DoubleArrayTrie {
 public:
  DoubleArrayTrie();
  ~DoubleArrayTrie();
  DoubleArrayTrie(const DoubleArrayTrie&) = delete;
  DoubleArrayTrie& operator=(const DoubleArrayTrie&) = delete;

  bool Load(const std::filesystem::path& path, bool verify = false) noexcept;
  bool Verify() const noexcept;
  bool IsAvailable() const noexcept;
  uint32_t LayerId() const noexcept;
  std::string_view Metadata() const noexcept;
  std::string_view Error() const noexcept;
  void CommonPrefixSearch(std::string_view key, size_t max_results,
                          std::vector<PrefixMatch>& out) const noexcept;
  void PredictiveSearch(std::string_view key, size_t max_results,
                        std::vector<PrefixMatch>& out) const noexcept;
  bool ExactMatch(std::string_view key, PrefixMatch& out) const noexcept;
  bool ReadEntries(const PrefixMatch& match,
                   std::vector<StaticDictionaryEntry>& out) const noexcept;
  // Exact surface lookup for reconversion, in ENTS order. Binary-searches the
  // optional SIDX section; artifacts without it fall back to scanning ENTS.
  void LookupSurface(std::string_view surface,
                     std::vector<StaticDictionaryEntry>& out) const noexcept;
  // Bytes of the section with the four-letter `type` (such as "CONN" or "CMAT"), or empty when
  // the artifact has none or is unavailable. The view is valid until the trie is loaded again.
  std::string_view Section(std::string_view type) const noexcept;
  // ENTS record count from the header, or 0 when unavailable.
  uint32_t EntryCount() const noexcept;
  // ReadEntries for the lattice: the same section 15.5 (2) checks, but strings are neither
  // copied nor UTF-8-checked. A violation invalidates the trie, as in ReadEntries.
  bool ReadEntryRefs(const PrefixMatch& match, std::vector<StaticEntryRef>& out) const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azookey::core
