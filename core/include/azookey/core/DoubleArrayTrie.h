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

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azookey::core
