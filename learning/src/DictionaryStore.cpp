#include "azookey/learning/DictionaryStore.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace azookey::learning {
namespace {
size_t IndexOf(LayerId layer) { return static_cast<size_t>(layer); }
double Score(const DictionaryEntry& entry, const LookupContext& ctx) {
  if (entry.source == LayerId::User && entry.use_user_word_score)
    return entry.user_word_score.value_or(ctx.user_word_default_score);
  double boost = 1.0;
  for (uint16_t category = 0; category < 10; ++category) {
    if (!(entry.category_mask & (1U << category))) continue;
    const auto it = ctx.category_boosts.find(category);
    const auto resolved =
        it != ctx.category_boosts.end() ? it->second
        : (category == 1 || category == 2 || category == 3 || category == 4 || category == 7)
            ? ctx.named_entity_boost
            : 1.0;
    if (std::isfinite(resolved)) boost = std::max(boost, std::clamp(resolved, 1.0, 1.2));
  }
  double penalty = 0;
  if (entry.source >= LayerId::User && entry.last_used && ctx.now_epoch_sec > entry.last_used) {
    const double days = static_cast<double>(ctx.now_epoch_sec - entry.last_used) / 86400.0;
    const double half_life = entry.category_mask & (1U << 8)   ? 120.0
                             : entry.category_mask & 254U      ? 90.0
                             : entry.category_mask & (1U << 9) ? 60.0
                                                               : 30.0;
    penalty = .10 * (1.0 - std::exp2(-days / half_life));
  }
  const double bonus = entry.kind == core::MatchKind::Exact   ? .10
                       : entry.kind == core::MatchKind::Alias ? .05
                                                              : .02;
  return entry.frequency + DictionaryStore::LayerPriority(entry.source, entry.confirmed) + bonus +
         boost - 1.0 - penalty;
}
}  // namespace

double DictionaryStore::LayerPriority(LayerId layer, bool confirmed) {
  constexpr std::array<double, 8> values{{.20, .30, .35, .45, .50, 1.0, .85, .70}};
  if (layer == LayerId::AutoWords && !confirmed) return .55;
  return IndexOf(layer) < values.size() ? values[IndexOf(layer)] : 0;
}
bool DictionaryStore::LoadStatic(LayerId layer, const std::filesystem::path& path, bool verify) {
  const auto index = IndexOf(layer);
  if (index >= static_.size()) return false;
  static_errors_[index].clear();
  auto trie = std::make_unique<core::DoubleArrayTrie>();
  const bool loaded = trie->Load(path, verify);
  if (loaded && trie->LayerId() != index) {
    static_[index].reset();
    static_errors_[index] = "layer id mismatch";
    return false;
  }
  static_[index] = std::move(trie);
  return loaded;
}
void DictionaryStore::EnableLayer(LayerId layer, bool enabled) {
  if (IndexOf(layer) < enabled_.size()) enabled_[IndexOf(layer)] = enabled;
}
bool DictionaryStore::IsAvailable(LayerId layer) const {
  const auto i = IndexOf(layer);
  return i < static_.size() ? static_[i] && static_[i]->IsAvailable()
                            : i < enabled_.size() && mutable_available_[i - 5];
}
std::string DictionaryStore::LayerError(LayerId layer) const {
  const auto i = IndexOf(layer);
  if (i < static_errors_.size() && !static_errors_[i].empty()) return static_errors_[i];
  return i < static_.size() && static_[i] ? std::string(static_[i]->Error()) : std::string{};
}
void DictionaryStore::ReplaceMutable(LayerId layer, std::vector<DictionaryEntry> entries) {
  const auto i = IndexOf(layer);
  if (i < 5 || i >= 8) return;
  Index next;
  for (auto& entry : entries) {
    entry.normalized_reading = core::NormalizeReading(entry.reading);
    if (entry.normalized_reading.empty() || entry.surface.empty() ||
        !core::IsValidUtf8(entry.surface) || !std::isfinite(entry.frequency) ||
        entry.frequency < 0 || entry.frequency > 1)
      continue;
    entry.source = layer;
    for (const auto& key : core::ReadingAliases(entry.normalized_reading)) {
      entry.kind =
          key == entry.normalized_reading ? core::MatchKind::Exact : core::MatchKind::Alias;
      next[key].push_back(entry);
    }
  }
  mutable_[i - 5] = std::move(next);
  mutable_available_[i - 5] = true;
}
void DictionaryStore::SetUserWords(const std::vector<UserWord>& words) {
  std::vector<DictionaryEntry> entries;
  entries.reserve(words.size());
  for (const auto& word : words) {
    if (word.value && !std::isfinite(*word.value)) continue;
    DictionaryEntry entry;
    entry.surface = word.word;
    entry.reading = word.ruby;
    entry.frequency = .4;
    entry.use_user_word_score = true;
    entry.user_word_score = word.value;
    entries.push_back(std::move(entry));
  }
  ReplaceMutable(LayerId::User, std::move(entries));
}
void DictionaryStore::QueryLayer(size_t layer, std::string_view key, const LookupContext& ctx,
                                 std::vector<DictionaryEntry>& out) const {
  out.clear();
  if (!enabled_[layer]) return;
  if (layer < 5) {
    const auto& trie = static_[layer];
    if (!trie || !trie->IsAvailable()) return;
    std::vector<core::PrefixMatch> matches;
    if (ctx.mode == LookupMode::Exact) {
      core::PrefixMatch match;
      if (trie->ExactMatch(key, match)) matches.push_back(match);
    } else if (ctx.mode == LookupMode::CommonPrefix)
      trie->CommonPrefixSearch(key, ctx.max_results, matches);
    else
      trie->PredictiveSearch(key,
                             ctx.exclude_exact_key && ctx.max_results &&
                                     ctx.max_results < std::numeric_limits<size_t>::max()
                                 ? ctx.max_results + 1
                                 : ctx.max_results,
                             matches);
    if (ctx.mode == LookupMode::PredictivePrefix && ctx.exclude_exact_key) {
      std::erase_if(matches, [&](const auto& match) { return match.key_length == key.size(); });
      if (ctx.max_results && matches.size() > ctx.max_results) matches.resize(ctx.max_results);
    }
    for (const auto& match : matches) {
      std::vector<core::StaticDictionaryEntry> entries;
      if (!trie->ReadEntries(match, entries)) {
        out.clear();
        return;
      }
      for (auto& entry : entries) {
        DictionaryEntry result;
        result.surface = std::move(entry.surface);
        result.reading = std::move(entry.reading);
        result.normalized_reading = core::NormalizeReading(result.reading);
        result.frequency = entry.frequency;
        result.category_mask = entry.category_mask;
        result.source = static_cast<LayerId>(entry.source);
        result.kind = entry.kind;
        out.push_back(std::move(result));
      }
    }
  } else {
    const auto& index = mutable_[layer - 5];
    std::vector<Index::const_iterator> matches;
    if (ctx.mode == LookupMode::Exact) {
      const auto it = index.find(std::string(key));
      if (it != index.end()) matches.push_back(it);
    } else if (ctx.mode == LookupMode::CommonPrefix) {
      for (size_t length = 1; length <= key.size(); ++length) {
        if (length != key.size() && (static_cast<uint8_t>(key[length]) & 0xc0) == 0x80) continue;
        const auto it = index.find(std::string(key.substr(0, length)));
        if (it != index.end()) matches.push_back(it);
      }
    } else {
      for (auto it = index.lower_bound(std::string(key));
           it != index.end() && it->first.starts_with(key); ++it)
        if (!ctx.exclude_exact_key || it->first != key) matches.push_back(it);
    }
    std::sort(matches.begin(), matches.end(), [](auto a, auto b) {
      return std::tuple(a->first.size(), a->first) < std::tuple(b->first.size(), b->first);
    });
    if (ctx.max_results && matches.size() > ctx.max_results) matches.resize(ctx.max_results);
    for (auto it : matches) out.insert(out.end(), it->second.begin(), it->second.end());
  }
}
std::vector<DictionaryEntry> DictionaryStore::Lookup(std::string_view reading,
                                                     const LookupContext& ctx) const {
  const auto key = core::NormalizeReading(reading);
  if (key.empty()) return {};
  std::map<std::pair<std::string, std::string>, DictionaryEntry> merged;
  for (size_t layer = 0; layer < enabled_.size(); ++layer) {
    if (ctx.excluded_layers & (1U << layer)) continue;
    std::vector<DictionaryEntry> found;
    QueryLayer(layer, key, ctx, found);
    if (found.empty() && key.find("ー") != std::string::npos) {
      auto relaxed = key;
      size_t position;
      while ((position = relaxed.find("ー")) != std::string::npos)
        relaxed.erase(position, std::string("ー").size());
      if (!relaxed.empty()) QueryLayer(layer, relaxed, ctx, found);
      for (auto& entry : found) entry.kind = core::MatchKind::LongVowelRelaxed;
    }
    for (auto& entry : found) {
      entry.sources = static_cast<uint16_t>(1U << layer);
      const auto identity = std::make_pair(entry.normalized_reading, entry.surface);
      const auto it = merged.find(identity);
      if (it == merged.end()) {
        merged.emplace(identity, std::move(entry));
        continue;
      }
      auto& previous = it->second;
      const auto categories = static_cast<uint16_t>(previous.category_mask | entry.category_mask);
      const auto sources = static_cast<uint16_t>(previous.sources | entry.sources);
      const double before = previous.frequency + LayerPriority(previous.source, previous.confirmed);
      const double after = entry.frequency + LayerPriority(entry.source, entry.confirmed);
      if (after > before || (after == before && entry.kind < previous.kind))
        previous = std::move(entry);
      previous.category_mask = categories;
      previous.sources = sources;
    }
  }
  std::vector<DictionaryEntry> result;
  for (auto& [key_pair, entry] : merged) {
    (void)key_pair;
    entry.score = Score(entry, ctx);
    if (!std::isfinite(entry.score)) continue;
    result.push_back(std::move(entry));
  }
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    if (a.score != b.score) return a.score > b.score;
    return std::tie(a.normalized_reading, a.surface, a.source) <
           std::tie(b.normalized_reading, b.surface, b.source);
  });
  return result;
}
}  // namespace azookey::learning
