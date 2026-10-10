#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "azookey/core/Candidate.h"
#include "azookey/host/EnglishDictionaryFiles.h"

namespace azookey::host {

// M60 inline English candidates (docs/inline-english-candidate-spec.md).
// Settings keys inlineEnglish* / fullWidthEnglishCandidate (section 7).
struct EnglishCandidateConfig {
  bool case_variants{true};
  bool full_width{false};
  uint32_t min_length{2};
  double promote_threshold{0.6};
  bool dictionary_enabled{false};
  std::string dictionary_path;  // UTF-8; a leading %LOCALAPPDATA% is expanded.
};

enum EnglishWordFlag : uint8_t {
  kEnglishWordProper = 1 << 0,
  kEnglishWordAcronym = 1 << 1,
  kEnglishWordTech = 1 << 2,
};

struct EnglishDictionaryEntry {
  std::string surface;
  uint64_t frequency{};
  uint8_t flags{};
};

struct EnglishDictionaryLoadStats {
  size_t skipped_lines{};  // Malformed or non-UTF-8 lines.
  bool truncated{false};   // Stopped at kMaxEnglishDictionaryEntries.
};

// Bounds memory for a large or hostile file; the rest of the file is ignored.
inline constexpr size_t kMaxEnglishDictionaryEntries = 200'000;

// Section 4.4 TSV: surface<TAB>frequency[<TAB>flags]. Records keyed by the
// lowercased surface, in file order; an exact duplicate surface keeps the last
// definition in the file at the place of the first. A frequency above
// UINT32_MAX saturates (the section 4.5 .bin field is 32-bit).
std::vector<EnglishWordRecord> ParseEnglishTsv(std::string_view text,
                                               EnglishDictionaryLoadStats* stats = nullptr);
// nullopt when the file is missing, unreadable or larger than 64 MiB.
std::optional<std::string> ReadEnglishTsvFile(const std::filesystem::path& path);

// A dictionary snapshot: the section 4.5 base with the section 4.6 overlay ops
// replayed over it (a later op on the same (key, surface) wins; delete hides
// the base entry). Within a key, entries are ordered by frequency, highest
// first.
class EnglishDictionary {
 public:
  EnglishDictionary() = default;
  explicit EnglishDictionary(std::shared_ptr<const EnglishBaseImage> base,
                             const std::vector<EnglishOverlayOp>& ops = {});
  static EnglishDictionary ParseTsv(std::string_view text,
                                    EnglishDictionaryLoadStats* stats = nullptr);
  static std::optional<EnglishDictionary> LoadTsv(const std::filesystem::path& path,
                                                  EnglishDictionaryLoadStats* stats = nullptr);

  std::vector<EnglishDictionaryEntry> Lookup(std::string_view lower_key) const;
  size_t size() const { return size_; }
  const std::shared_ptr<const EnglishBaseImage>& base() const { return base_; }

 private:
  std::shared_ptr<const EnglishBaseImage> base_;
  // key -> the latest op per surface.
  std::unordered_map<std::string, std::vector<EnglishOverlayOp>> overlay_;
  size_t size_{0};
};

// Section 4.2 signals, each in [0, 1].
struct EnglishIntent {
  double dict{};
  double nonkana{};
  double cluster{};
  double length{};
  double upper{};
  double Score() const;
};

EnglishIntent ComputeEnglishIntent(std::string_view raw_romaji, bool dictionary_hit,
                                   uint32_t min_length);

struct EnglishCandidates {
  std::vector<core::Candidate> candidates;  // Section 4.3 fixed order, tag English.
  double intent{};
};

// Builds the candidates for raw_romaji. Empty when it is shorter than
// min_length, has no ASCII letter, or holds anything but printable ASCII.
// learned are surfaces the English learning channel holds for lower(raw),
// strongest first; they follow the dictionary surfaces.
EnglishCandidates BuildEnglishCandidates(std::string_view raw_romaji,
                                         const EnglishCandidateConfig& config,
                                         const EnglishDictionary* dictionary,
                                         const std::vector<std::string>& learned = {});

// Section 4.3: inserts english after the first candidate when intent reaches
// the threshold, otherwise after the first five; surfaces already present are
// skipped. Never takes the first slot from a non-empty list.
void PlaceEnglishCandidates(std::vector<core::Candidate>& candidates,
                            std::vector<core::Candidate> english, double intent,
                            double promote_threshold);

// The learning and dictionary key: raw_romaji with ASCII letters lowercased.
std::string EnglishLookupKey(std::string_view raw_romaji);

// Section 6.4: a commit belongs to the English channel only when the chosen
// candidate carries the English tag AND its reading is printable ASCII (the
// raw romaji). The surface-form English tag also lands on kana-read dictionary
// words such as iPhone (reading あいふぉん); those stay in the kana channel.
bool IsEnglishObservation(std::string_view reading, uint8_t tag);

}  // namespace azookey::host
