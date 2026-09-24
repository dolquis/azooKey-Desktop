#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/learning/DpapiCrypto.h"

namespace azookey::learning {

inline constexpr std::string_view kAutoWordStoreTsvHeader = "# azookey-auto-word-store v1";

// docs/auto-word-registration-spec.md section 3-3: pending entries that never
// reach confirmation are swept after this long.
inline constexpr uint64_t kAutoWordDefaultPendingMaxAgeSec = 90ULL * 24 * 60 * 60;

enum class AutoWordSource { Mining, Trending };
enum class AutoWordState { Pending, Confirmed, Rejected };

struct AutoWord {
  std::string surface;
  std::string reading;
  AutoWordSource source{AutoWordSource::Mining};
  AutoWordState state{AutoWordState::Pending};
  // Observation frequency for mined words; a rank-derived seed for trending ones.
  uint32_t count{0};
  uint64_t first_seen_epoch{0};
  uint64_t last_seen_epoch{0};
  // Score used when the word is eventually injected as a candidate.
  double score{0.0};
};

std::string_view AutoWordSourceName(AutoWordSource source);
std::string_view AutoWordStateName(AutoWordState state);
bool ParseAutoWordSource(std::string_view value, AutoWordSource& out);
bool ParseAutoWordState(std::string_view value, AutoWordState& out);

// Words the host has seen but that no dictionary layer knows, held until they
// are confirmed or rejected. Keyed by (surface, reading).
//
// Persisted as encrypted TSV at path + ".enc", using the LearningStore
// escaping convention:
//
//   # azookey-auto-word-store v1
//   # surface<TAB>reading<TAB>source<TAB>state<TAB>count<TAB>first_seen_epoch<TAB>...
//   azooKey<TAB>あずきー<TAB>mining<TAB>pending<TAB>2<TAB>100<TAB>200<TAB>0
//
// The store owns a mutex: spec section 3-3 has TrendingWordFetcher (M36-B)
// updating it from a worker thread while the dispatcher reads it.
class AutoWordStore {
 public:
  explicit AutoWordStore(std::filesystem::path path, const ByteCrypto* crypto = nullptr);

  // Missing file -> empty store, returns true. Malformed rows are skipped.
  bool Load();
  bool Save() const;
  void Reset();

  // Records one mining observation. A new key is added as Pending; an existing
  // key has its count incremented. A rejected key is ignored entirely, count
  // included, so a word the user turned down is never offered again.
  // Returns true only when this call promoted the word to Confirmed.
  bool Observe(const std::string& surface, const std::string& reading, uint64_t now_epoch,
               uint32_t promote_threshold, bool auto_promote);

  // Bulk import of a downloaded trending list (M36-B). Rejected keys are
  // skipped; a key that already exists as a mined word keeps source Mining,
  // because a local observation outranks a remote list.
  void IngestTrending(const std::vector<AutoWord>& batch, uint64_t now_epoch, bool auto_promote);

  std::vector<AutoWord> ListByState(AutoWordState state) const;
  bool Confirm(const std::string& surface, const std::string& reading);
  bool Reject(const std::string& surface, const std::string& reading);
  // Moves the word to `state` whatever it was before and returns the previous
  // state, or nullopt when the key is absent. The approval handler uses the
  // previous state to tell an idempotent repeat from a change and to roll the
  // change back when Save() fails.
  std::optional<AutoWordState> SetState(const std::string& surface, const std::string& reading,
                                        AutoWordState state);
  // Moves the word from `expected` to `desired` only if it is still in
  // `expected`. A rollback uses this so it cannot undo a decision another
  // connection made in the meantime. Returns whether the state was changed.
  bool CompareAndSetState(const std::string& surface, const std::string& reading,
                          AutoWordState expected, AutoWordState desired);

  // Confirmed words for one reading; pending and rejected words never surface.
  std::vector<AutoWord> LookupConfirmed(const std::string& reading) const;

  // Drops pending entries older than max_age_sec. Returns how many were removed.
  size_t PrunePending(uint64_t now_epoch, uint64_t max_age_sec);

  size_t Size() const;
  const std::filesystem::path& path() const { return path_; }

 private:
  AutoWord* FindLocked(const std::string& surface, const std::string& reading);
  bool SetStateLocked(const std::string& surface, const std::string& reading, AutoWordState state);

  mutable std::mutex mutex_;
  std::filesystem::path path_;
  const ByteCrypto* crypto_;
  bool save_blocked_by_load_failure_{false};
  // reading -> surface -> word. Gives (surface, reading) uniqueness and the
  // reading-keyed lookup LookupConfirmed needs from one container.
  std::map<std::string, std::map<std::string, AutoWord>> table_;
};

}  // namespace azookey::learning
