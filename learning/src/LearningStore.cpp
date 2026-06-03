#include "azookey/learning/LearningStore.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

#include "AtomicFile.h"

namespace azookey::learning {

namespace {
double DecayedWeight(const LearningRecord& rec, uint64_t now_epoch_sec) {
  const double elapsed_seconds =
      now_epoch_sec >= rec.last_updated_epoch_sec
          ? static_cast<double>(now_epoch_sec - rec.last_updated_epoch_sec)
          : 0.0;
  const double days = elapsed_seconds / (60.0 * 60.0 * 24.0);
  const double decay = std::exp(-0.15 * days);
  return rec.weight * decay;
}
}  // namespace

LearningStore::LearningStore(std::string path) : path_(std::move(path)) {}

std::string LearningStore::Key(const std::string& reading, const std::string& surface) const {
  return reading + "\t" + surface;
}

bool LearningStore::Load() {
  table_.clear();
  dirty_ = false;
  std::ifstream ifs(path_);
  if (!ifs.is_open()) {
    return false;
  }
  std::string line;
  while (std::getline(ifs, line)) {
    std::istringstream iss(line);
    std::string reading;
    std::string surface;
    LearningRecord rec;
    if (!(std::getline(iss, reading, '\t') && std::getline(iss, surface, '\t') &&
          (iss >> rec.weight) && (iss >> rec.last_updated_epoch_sec))) {
      continue;
    }
    table_.emplace(Key(reading, surface), rec);
  }
  return true;
}

bool LearningStore::Save() const {
  std::ostringstream out;
  std::vector<std::string> keys;
  keys.reserve(table_.size());
  for (const auto& [key, _] : table_) {
    keys.push_back(key);
  }
  std::sort(keys.begin(), keys.end());
  for (const auto& key : keys) {
    const auto& rec = table_.at(key);
    const auto tab = key.find('\t');
    if (tab == std::string::npos) {
      continue;
    }
    out << key.substr(0, tab) << '\t' << key.substr(tab + 1) << '\t' << rec.weight << ' '
        << rec.last_updated_epoch_sec << '\n';
  }
  const bool saved = WriteTextFileAtomically(path_, out.str());
  if (saved) {
    dirty_ = false;
  }
  return saved;
}

void LearningStore::Reset() {
  if (!table_.empty()) {
    dirty_ = true;
  }
  table_.clear();
}

bool LearningStore::dirty() const { return dirty_; }

size_t LearningStore::size() const { return table_.size(); }

void LearningStore::Observe(const std::string& reading, const std::string& surface, double alpha,
                            uint64_t now_epoch_sec) {
  auto& rec = table_[Key(reading, surface)];
  rec.weight += alpha;
  rec.last_updated_epoch_sec = now_epoch_sec;
  dirty_ = true;
}

void LearningStore::ObserveCorrection(const std::string& reading,
                                      const std::string& rejected_surface,
                                      const std::string& selected_surface, double alpha,
                                      uint64_t now_epoch_sec) {
  Observe(reading, selected_surface, alpha, now_epoch_sec);

  auto& rejected = table_[Key(reading, rejected_surface)];
  rejected.weight = std::max(0.0, rejected.weight - alpha);
  rejected.last_updated_epoch_sec = now_epoch_sec;
  dirty_ = true;
}

void LearningStore::Prune(size_t max_records, double min_weight, uint64_t now_epoch_sec) {
  bool changed = false;
  if (min_weight > 0.0) {
    for (auto it = table_.begin(); it != table_.end();) {
      if (DecayedWeight(it->second, now_epoch_sec) < min_weight) {
        it = table_.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }
  }

  if (max_records > 0 && table_.size() > max_records) {
    std::vector<std::pair<std::string, double>> ranked;
    ranked.reserve(table_.size());
    for (const auto& [key, rec] : table_) {
      ranked.emplace_back(key, DecayedWeight(rec, now_epoch_sec));
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.second == rhs.second) {
        return lhs.first < rhs.first;
      }
      return lhs.second < rhs.second;
    });

    const size_t remove_count = table_.size() - max_records;
    for (size_t i = 0; i < remove_count; ++i) {
      table_.erase(ranked[i].first);
    }
    changed = remove_count > 0;
  }

  if (changed) {
    dirty_ = true;
  }
}

double LearningStore::Score(const std::string& reading, const std::string& surface,
                            uint64_t now_epoch_sec) const {
  const auto it = table_.find(Key(reading, surface));
  if (it == table_.end()) {
    return 0.0;
  }
  return DecayedWeight(it->second, now_epoch_sec);
}

}  // namespace azookey::learning
