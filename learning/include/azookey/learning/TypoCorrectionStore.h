#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::learning {

inline constexpr std::string_view kTypoCorrectionStoreEscapedTsvHeader =
    "# azookey-typo-correction-tsv escaped=1";

// docs/typo-correction-learning-spec.md section 6: a pair is only applied once
// it has been seen this many times, so a single mis-detected correction never
// reaches the candidate list.
inline constexpr uint32_t kTypoCorrectionDefaultMinCount = 3;

// Section 5-1 leaves the cutoff as an example ("180 days"); it is fixed here so
// Lookup has one definition of "too old to trust".
inline constexpr uint64_t kTypoCorrectionMaxRecordAgeSec = 180ULL * 24 * 60 * 60;

// Section 6 accept filters, in UTF-8 code points rather than bytes.
inline constexpr size_t kTypoCorrectionMinReadingLength = 2;
// Upper bound on a reading the store will consider, in code points. The accept
// filter runs an O(n*m) edit distance, and the readings arrive from the TIP over
// IPC where a frame may carry up to a megabyte: without this cap one crafted
// ObserveTypo could occupy the store's lock for the length of a quadratic scan
// over hundreds of thousands of code points. A mistyped reading is a preedit,
// so anything beyond this is not a typo pair regardless.
inline constexpr size_t kTypoCorrectionMaxReadingLength = 64;
inline constexpr size_t kTypoCorrectionMaxEditDistance = 3;
inline constexpr double kTypoCorrectionEditDistanceRatio = 0.34;

struct TypoCorrectionRecord {
  uint32_t count{};
  uint64_t last_updated_epoch_sec{};
};

struct TypoCorrectionEntry {
  std::string wrong_reading;
  std::string correct_reading;
  TypoCorrectionRecord record;
};

// Frequency table of (wrong reading -> correct reading) pairs observed when the
// user retypes a reading. Backed by a TSV file that follows the LearningStore
// separator and escaping convention:
//
//   # azookey-typo-correction-tsv escaped=1
//   wrong_reading<TAB>correct_reading<TAB>count last_updated_epoch
//
// Like LearningStore, the caller serializes reads and writes; the store holds no
// lock of its own.
class TypoCorrectionStore {
 public:
  explicit TypoCorrectionStore(std::filesystem::path path);

  // Counts the pair when it passes the section 6 accept filters. Returns false
  // (recording nothing) for a pair that is out of range.
  bool Observe(const std::string& wrong, const std::string& correct, uint64_t now_epoch_sec);

  // The most frequently observed correct reading for `wrong`, provided its count
  // reached `min_count`. Ties go to the lexicographically smaller reading so the
  // result does not depend on insertion order.
  std::optional<std::string> Lookup(const std::string& wrong,
                                    uint32_t min_count = kTypoCorrectionDefaultMinCount) const;

  // As above, but also ignores records whose last_updated is more than
  // kTypoCorrectionMaxRecordAgeSec behind `now_epoch_sec` (section 5-1). This is
  // the overload the host calls; the two-argument form keeps the spec signature
  // for callers that have no clock.
  std::optional<std::string> Lookup(const std::string& wrong, uint32_t min_count,
                                    uint64_t now_epoch_sec) const;

  // Missing file -> empty store, returns true: a store that has never been
  // written is not an error. Malformed rows are skipped so one bad line does not
  // discard the rest of the table.
  bool Load();
  bool Save() const;
  void Reset();

  size_t size() const;
  std::vector<TypoCorrectionEntry> All() const;
  const std::filesystem::path& path() const { return path_; }

  // Section 5-1: kana occupies three bytes in UTF-8, so both helpers work in
  // code points. Invalid sequences are consumed one byte at a time, matching
  // core's UTF-8 decoding behaviour.
  static size_t Utf8CharLength(std::string_view value);
  static size_t Utf8EditDistance(std::string_view a, std::string_view b);

  // Section 6 accept filters, exposed so the edit-distance policy has one home.
  static bool IsLearnablePair(const std::string& wrong, const std::string& correct);
  static size_t EditDistanceLimit(size_t reading_length);

 private:
  std::filesystem::path path_;
  std::map<std::string, std::map<std::string, TypoCorrectionRecord>> table_;
};

}  // namespace azookey::learning
