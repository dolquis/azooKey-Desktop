#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "azookey/core/DoubleArrayTrie.h"
#include "azookey/learning/UserDictionary.h"

namespace azookey::learning {
enum class LayerId : uint8_t {
  Base,
  Sudachi,
  Neologd,
  NamedEntity,
  TechnicalTerms,
  User,
  AutoWords,
  AppSpecific
};
enum class LookupMode { Exact, CommonPrefix, PredictivePrefix };

struct DictionaryEntry {
  std::string surface;
  std::string reading;
  std::string normalized_reading;
  double frequency{};
  uint16_t category_mask{};
  LayerId source{LayerId::Base};
  uint16_t sources{};
  core::MatchKind kind{core::MatchKind::Exact};
  bool confirmed{true};
  uint64_t last_used{};
  double score{};
};
struct LookupContext {
  LookupMode mode{LookupMode::Exact};
  size_t max_results{32};
  uint64_t now_epoch_sec{};
  uint16_t excluded_layers{};
  // Category ids follow auto-word-registration-spec section 14.4.
  std::map<uint16_t, double> category_boosts;
  double named_entity_boost{1.0};
};

// Caller serializes reads and updates. Mutable persistence remains with its owner.
class DictionaryStore {
 public:
  bool LoadStatic(LayerId layer, const std::filesystem::path& path, bool verify = false);
  void ReplaceMutable(LayerId layer, std::vector<DictionaryEntry> entries);
  void SetUserWords(const std::vector<UserWord>& words);
  void EnableLayer(LayerId layer, bool enabled);
  bool IsAvailable(LayerId layer) const;
  std::string LayerError(LayerId layer) const;
  static double LayerPriority(LayerId layer, bool confirmed = true);
  std::vector<DictionaryEntry> Lookup(std::string_view reading, const LookupContext& context) const;

 private:
  using Index = std::map<std::string, std::vector<DictionaryEntry>>;
  void QueryLayer(size_t layer, std::string_view key, const LookupContext& context,
                  std::vector<DictionaryEntry>& out) const;
  std::array<std::unique_ptr<core::DoubleArrayTrie>, 5> static_;
  std::array<Index, 3> mutable_;
  std::array<bool, 8> enabled_{{true, true, false, true, true, true, true, true}};
};
}  // namespace azookey::learning
