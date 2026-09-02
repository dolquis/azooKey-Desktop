#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::learning {

inline constexpr std::string_view kLearningStoreEscapedTsvHeader =
    "# azookey-learning-tsv escaped=1";

struct LearningRecord {
  double weight{};
  uint64_t last_updated_epoch_sec{};
};

struct LearningEntry {
  std::string reading;
  std::string surface;
  LearningRecord record;
};

struct PrefixMatch {
  std::string reading;
  std::string surface;
  double score{};
};

struct PrefixLookupResult {
  std::vector<PrefixMatch> matches;
  size_t visited_readings{};
  size_t scanned_records{};
};

class LearningStore {
 public:
  explicit LearningStore(std::string path);
  virtual ~LearningStore() = default;

  bool Load();
  bool Save() const;
  void Reset();
  bool dirty() const;
  size_t size() const;
  std::vector<LearningEntry> All() const;
  PrefixLookupResult LookupPrefix(const std::string& reading_prefix, size_t limit, double min_score,
                                  uint64_t now_epoch_sec) const;

  void Observe(const std::string& reading, const std::string& surface, double alpha,
               uint64_t now_epoch_sec);
  void ObserveCorrection(const std::string& reading, const std::string& rejected_surface,
                         const std::string& selected_surface, double alpha, uint64_t now_epoch_sec);
  void Prune(size_t max_records, double min_weight, uint64_t now_epoch_sec);
  virtual double Score(const std::string& reading, const std::string& surface,
                       uint64_t now_epoch_sec) const;

 private:
  std::string path_;
  std::map<std::string, std::map<std::string, LearningRecord>> table_;
  mutable bool dirty_{false};
};

}  // namespace azookey::learning
