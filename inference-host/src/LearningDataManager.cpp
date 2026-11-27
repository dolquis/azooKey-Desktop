#include "azookey/host/LearningDataManager.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "azookey/learning/Sha256.h"

namespace azookey::host {

namespace detail {
bool LearningStoreSelected(const std::vector<LearningDataStore>& selected,
                           LearningDataStore store) {
  return std::find(selected.begin(), selected.end(), store) != selected.end();
}

bool UserDictionaryFileExists(const learning::UserDictionary& dict) {
  std::error_code ec;
  if (std::filesystem::exists(learning::EncryptedPathFor(dict.path()), ec) && !ec) {
    return true;
  }
  ec.clear();
  return std::filesystem::exists(dict.path(), ec) && !ec;
}
}  // namespace detail

namespace {
constexpr size_t kEntryIdHexDigits = 16;

bool Matches(std::string_view query, const std::string& reading, const std::string& surface) {
  return query.empty() || reading.find(query) != std::string::npos ||
         surface.find(query) != std::string::npos;
}

void AppendLearning(LearningDataStore store, const LearningChannel& channel,
                    std::vector<LearningDataEntry>& out) {
  if (!channel.store) return;
  for (auto& aggregate : channel.store->Aggregates()) {
    LearningDataEntry entry;
    entry.id = LearningEntryId(store, channel.name, aggregate.reading, aggregate.surface);
    entry.channel = channel.name;
    entry.reading = std::move(aggregate.reading);
    entry.surface = std::move(aggregate.surface);
    entry.weight = aggregate.weight;
    entry.last_updated_epoch_sec = aggregate.last_updated_epoch_sec;
    entry.tags = std::move(aggregate.app_names);
    entry.metadata.emplace("commit_count", std::to_string(aggregate.commit_count));
    entry.metadata.emplace("accept_count", std::to_string(aggregate.accept_count));
    entry.metadata.emplace("reject_count", std::to_string(aggregate.reject_count));
    out.push_back(std::move(entry));
  }
}

// A pattern is one entry per (observed, intended), whatever its typo types
// and keyboard layouts, since forgetting removes them together
// (typo-correction-learning-spec section 12.8).
void AppendTypoPatterns(const learning::TypoLearningStore& typo,
                        std::vector<LearningDataEntry>& out) {
  struct Folded {
    uint64_t accept_count{};
    uint64_t reject_count{};
    uint64_t last_updated_epoch_sec{};
  };
  std::map<std::pair<std::string, std::string>, Folded> folded;
  for (auto& pattern : typo.Patterns()) {
    auto& row = folded[{std::move(pattern.observed_pattern), std::move(pattern.intended_pattern)}];
    row.accept_count += pattern.accept_count;
    row.reject_count += pattern.reject_count;
    row.last_updated_epoch_sec = (std::max)(row.last_updated_epoch_sec, pattern.updated_at);
  }
  for (auto& [key, row] : folded) {
    LearningDataEntry entry;
    entry.id = LearningEntryId(LearningDataStore::Typo, kTypoPatternChannel, key.first, key.second);
    entry.channel = std::string(kTypoPatternChannel);
    entry.reading = key.first;
    entry.surface = key.second;
    entry.weight = static_cast<double>(row.accept_count);
    entry.last_updated_epoch_sec = row.last_updated_epoch_sec;
    entry.metadata.emplace("accept_count", std::to_string(row.accept_count));
    entry.metadata.emplace("reject_count", std::to_string(row.reject_count));
    out.push_back(std::move(entry));
  }
}

// Counts what an item holds and erases the copied readings.
size_t CountTypoReadingPairs(const learning::TypoLearningStore& typo) {
  auto pairs = typo.ReadingPairs();
  const size_t count = pairs.size();
  for (auto& pair : pairs) {
    learning::SecureErase(pair.wrong_reading);
    learning::SecureErase(pair.correct_reading);
  }
  return count;
}

void EraseTypoPatterns(std::vector<learning::TypoPatternRecord>& patterns) {
  for (auto& pattern : patterns) {
    learning::SecureErase(pattern.observed_pattern);
    learning::SecureErase(pattern.intended_pattern);
  }
}

size_t CountTypoPatterns(const learning::TypoLearningStore& typo) {
  auto patterns = typo.Patterns();
  const size_t count = patterns.size();
  EraseTypoPatterns(patterns);
  return count;
}

std::vector<LearningDataEntry> CollectEntries(const LearningDataStores& stores,
                                              LearningDataStore store) {
  std::vector<LearningDataEntry> entries;
  switch (store) {
    case LearningDataStore::Learning:
      for (const auto& channel : stores.learning) AppendLearning(store, channel, entries);
      break;
    case LearningDataStore::UserDictionary:
      if (!stores.user_dictionary) break;
      for (const auto& word : stores.user_dictionary->All()) {
        LearningDataEntry entry;
        entry.id = LearningEntryId(store, "", word.ruby, word.word);
        entry.reading = word.ruby;
        entry.surface = word.word;
        entry.weight = word.value.value_or(0.0);
        if (word.cid) entry.metadata.emplace("cid", std::to_string(*word.cid));
        if (word.mid) entry.metadata.emplace("mid", std::to_string(*word.mid));
        // user-dictionary-pos-spec 7.3.
        if (word.pos) entry.metadata.emplace("pos", *word.pos);
        entries.push_back(std::move(entry));
      }
      break;
    case LearningDataStore::Typo:
      if (!stores.typo) break;
      for (const auto& typo : stores.typo->ReadingPairs()) {
        LearningDataEntry entry;
        entry.id = LearningEntryId(store, "", typo.wrong_reading, typo.correct_reading);
        entry.reading = typo.wrong_reading;
        entry.surface = typo.correct_reading;
        entry.weight = static_cast<double>(typo.record.count);
        entry.last_updated_epoch_sec = typo.record.last_updated_epoch_sec;
        entries.push_back(std::move(entry));
      }
      AppendTypoPatterns(*stores.typo, entries);
      break;
    case LearningDataStore::AutoWord:
      if (!stores.auto_word) break;
      for (const auto& word : stores.auto_word->All()) {
        LearningDataEntry entry;
        entry.id = LearningEntryId(store, "", word.reading, word.surface);
        entry.reading = word.reading;
        entry.surface = word.surface;
        entry.weight = word.score;
        entry.last_updated_epoch_sec = word.last_seen_epoch;
        entry.tags = {std::string(learning::AutoWordStateName(word.state)),
                      std::string(learning::AutoWordSourceName(word.source))};
        entry.metadata.emplace("count", std::to_string(word.count));
        entries.push_back(std::move(entry));
      }
      break;
  }
  return entries;
}

bool ForgetEntry(const LearningDataStores& stores, LearningDataStore store,
                 const LearningDataEntry& entry) {
  switch (store) {
    case LearningDataStore::Learning:
      for (const auto& channel : stores.learning) {
        if (channel.name == entry.channel && channel.store) {
          return channel.store->Forget(entry.reading, entry.surface);
        }
      }
      return false;
    case LearningDataStore::UserDictionary:
      return stores.user_dictionary && stores.user_dictionary->Remove(entry.surface, entry.reading);
    case LearningDataStore::Typo: {
      if (!stores.typo) return false;
      const auto result =
          entry.channel == kTypoPatternChannel
              ? stores.typo->ForgetPattern(entry.reading, entry.surface, stores.now_epoch_sec)
              : stores.typo->ForgetReadingPair(entry.reading, entry.surface, stores.now_epoch_sec);
      return result == learning::TypoForgetResult::Removed;
    }
    case LearningDataStore::AutoWord:
      return stores.auto_word && stores.auto_word->Reject(entry.surface, entry.reading);
  }
  return false;
}

// Archive item names follow learning-data-management-spec section 5.
std::string LearningItemName(const std::string& channel) {
  return channel == kKanaLearningChannel ? "learning" : "learning_" + channel;
}

std::string ItemFile(std::string base, bool encrypt) {
  if (encrypt) base += ".enc";
  return base;
}

struct ParsedLearning {
  std::string item_name;
  learning::LearningStore* target{};
  std::unique_ptr<learning::LearningStore> scratch;
};

struct ParsedItems {
  std::vector<ParsedLearning> learning;
  std::unique_ptr<learning::UserDictionary> user_dictionary;
  std::unique_ptr<learning::TypoCorrectionStore> typo;
  std::optional<std::vector<learning::TypoPatternRecord>> typo_patterns;
  std::unique_ptr<learning::AutoWordStore> auto_word;
};

bool AnySelectedStoreBlocked(const LearningDataStores& stores,
                             const std::vector<LearningDataStore>& selected) {
  if (detail::LearningStoreSelected(selected, LearningDataStore::Learning)) {
    for (const auto& channel : stores.learning) {
      if (channel.store && channel.store->save_blocked()) return true;
    }
  }
  return (detail::LearningStoreSelected(selected, LearningDataStore::UserDictionary) &&
          stores.user_dictionary && stores.user_dictionary->save_blocked()) ||
         (detail::LearningStoreSelected(selected, LearningDataStore::Typo) && stores.typo &&
          stores.typo->save_blocked()) ||
         (detail::LearningStoreSelected(selected, LearningDataStore::AutoWord) &&
          stores.auto_word && stores.auto_word->save_blocked());
}

const BackupItem* FindItem(const std::vector<BackupItem>& items, std::string_view name) {
  const auto it = std::find_if(items.begin(), items.end(),
                               [&](const BackupItem& item) { return item.name == name; });
  return it == items.end() ? nullptr : &*it;
}
}  // namespace

std::string_view LearningDataStoreName(LearningDataStore store) {
  switch (store) {
    case LearningDataStore::Learning:
      return "learning";
    case LearningDataStore::UserDictionary:
      return "user_dict";
    case LearningDataStore::Typo:
      return "typo";
    case LearningDataStore::AutoWord:
      return "auto_word";
  }
  return "learning";
}

std::optional<LearningDataStore> ParseLearningDataStore(std::string_view name) {
  for (const auto store : {LearningDataStore::Learning, LearningDataStore::UserDictionary,
                           LearningDataStore::Typo, LearningDataStore::AutoWord}) {
    if (LearningDataStoreName(store) == name) return store;
  }
  return std::nullopt;
}

std::string LearningEntryId(LearningDataStore store, std::string_view channel,
                            std::string_view reading, std::string_view surface) {
  // Length prefixes keep the key unambiguous whatever bytes the fields hold.
  std::string key;
  for (const auto field : {LearningDataStoreName(store), channel, reading, surface}) {
    key += std::to_string(field.size());
    key += ':';
    key += field;
  }
  return learning::Sha256Hex(key).substr(0, kEntryIdHexDigits);
}

LearningDataPage ListLearningEntries(const LearningDataStores& stores, LearningDataStore store,
                                     std::string_view query, size_t offset, size_t limit) {
  LearningDataPage page;
  limit = std::min(limit, kMaxLearningListLimit);
  for (auto& entry : CollectEntries(stores, store)) {
    if (!Matches(query, entry.reading, entry.surface)) continue;
    if (page.total >= offset && page.entries.size() < limit) {
      page.entries.push_back(std::move(entry));
    }
    ++page.total;
  }
  return page;
}

std::optional<LearningDataEntry> FindLearningEntry(const LearningDataStores& stores,
                                                   LearningDataStore store, std::string_view id) {
  for (auto& entry : CollectEntries(stores, store)) {
    if (entry.id == id) return std::move(entry);
  }
  return std::nullopt;
}

bool ForgetLearningEntry(const LearningDataStores& stores, LearningDataStore store,
                         std::string_view id) {
  for (const auto& entry : CollectEntries(stores, store)) {
    if (entry.id == id) return ForgetEntry(stores, store, entry);
  }
  return false;
}

bool ForgetLearningPair(const LearningDataStores& stores, const std::string& reading,
                        const std::string& surface) {
  for (const auto& channel : stores.learning) {
    if (channel.name == kKanaLearningChannel && channel.store) {
      return channel.store->Forget(reading, surface);
    }
  }
  return false;
}

std::optional<std::vector<BackupItem>> CollectBackupItems(
    const LearningDataStores& stores, const std::vector<LearningDataStore>& selected, bool encrypt,
    BackupError* error) {
  if (AnySelectedStoreBlocked(stores, selected)) {
    if (error) *error = BackupError::StoreUnavailable;
    return std::nullopt;
  }
  std::vector<BackupItem> items;
  if (detail::LearningStoreSelected(selected, LearningDataStore::Learning)) {
    for (const auto& channel : stores.learning) {
      if (!channel.store) continue;
      const auto name = LearningItemName(channel.name);
      auto text = channel.store->SerializeText();
      // Forgotten rows stay in memory until saved but are not written; count
      // what the item actually holds.
      const auto rows = learning::CountValidLearningRows(text).value_or(0);
      items.push_back(BackupItem{name, ItemFile(name + ".tsv", encrypt), rows, std::move(text)});
    }
  }
  if (detail::LearningStoreSelected(selected, LearningDataStore::UserDictionary) &&
      stores.user_dictionary) {
    items.push_back(BackupItem{"user_dictionary", ItemFile("user_dict.json", encrypt),
                               stores.user_dictionary->Size(),
                               stores.user_dictionary->SerializeText()});
  }
  if (detail::LearningStoreSelected(selected, LearningDataStore::Typo) && stores.typo) {
    items.push_back(BackupItem{"typo_corrections", ItemFile("typo_corrections.tsv", encrypt),
                               CountTypoReadingPairs(*stores.typo),
                               stores.typo->SerializeReadingPairsText()});
    // Only the v2 store has patterns (spec section 5).
    if (stores.typo->mode() == learning::TypoLearningMode::V2) {
      items.push_back(BackupItem{"typo_patterns", ItemFile("typo_patterns.tsv", encrypt),
                                 CountTypoPatterns(*stores.typo),
                                 stores.typo->SerializePatternsText()});
    }
  }
  if (detail::LearningStoreSelected(selected, LearningDataStore::AutoWord) && stores.auto_word) {
    items.push_back(BackupItem{"auto_words", ItemFile("auto_words.tsv", encrypt),
                               stores.auto_word->Size(), stores.auto_word->SerializeText()});
  }
  if (error) *error = BackupError::None;
  return items;
}

LearningExportResult WriteLearningBackup(std::vector<BackupItem> items,
                                         const std::filesystem::path& destination, bool encrypt,
                                         const learning::ByteCrypto& crypto,
                                         const BackupManifest& meta) {
  LearningExportResult result;
  WriteBackupArchive(destination, items, encrypt, crypto, meta, &result.manifest, &result.file_size,
                     &result.error);
  for (auto& item : items) learning::SecureErase(item.plaintext);
  return result;
}

LearningExportResult ExportLearningData(const LearningDataStores& stores,
                                        const std::vector<LearningDataStore>& selected,
                                        const std::filesystem::path& destination, bool encrypt,
                                        const learning::ByteCrypto& crypto,
                                        const BackupManifest& meta) {
  LearningExportResult result;
  auto items = CollectBackupItems(stores, selected, encrypt, &result.error);
  if (!items) return result;
  return WriteLearningBackup(std::move(*items), destination, encrypt, crypto, meta);
}

LearningImportResult ImportLearningData(const LearningDataStores& stores,
                                        const std::vector<LearningDataStore>& selected,
                                        const std::filesystem::path& source,
                                        learning::ImportConflictPolicy policy,
                                        const learning::ByteCrypto& crypto) {
  LearningImportResult result;
  // A store that refuses to save would drop the merge silently.
  if (AnySelectedStoreBlocked(stores, selected)) {
    result.error = BackupError::StoreUnavailable;
    return result;
  }
  BackupManifest manifest;
  auto items = ReadBackupArchive(source, crypto, &manifest, &result.error);
  if (!items) return result;
  return ApplyImportedItems(stores, selected, std::move(*items), policy);
}

LearningImportResult ApplyImportedItems(const LearningDataStores& stores,
                                        const std::vector<LearningDataStore>& selected,
                                        std::vector<BackupItem> items_in,
                                        learning::ImportConflictPolicy policy) {
  LearningImportResult result;
  if (AnySelectedStoreBlocked(stores, selected)) {
    for (auto& item : items_in) learning::SecureErase(item.plaintext);
    result.error = BackupError::StoreUnavailable;
    return result;
  }
  // Scratch stores never touch a file, so they need no real key.
  const learning::ByteCrypto* crypto = nullptr;
  auto* items = &items_in;
  // A row that does not parse means the item is not what this Host wrote;
  // importing the rest would lose data silently.
  // The first failure is the one reported.
  const auto parsed_or_fail = [&](bool ok) {
    if (!ok && result.error == BackupError::None) result.error = BackupError::BadItem;
  };

  // Parse everything first: a store is changed only once the whole archive
  // is known to be usable.
  ParsedItems parsed;
  if (detail::LearningStoreSelected(selected, LearningDataStore::Learning)) {
    for (const auto& channel : stores.learning) {
      const auto name = LearningItemName(channel.name);
      const auto* item = channel.store ? FindItem(*items, name) : nullptr;
      if (!item) continue;
      auto scratch = std::make_unique<learning::LearningStore>(std::filesystem::path{}, crypto);
      parsed_or_fail(scratch->LoadText(item->plaintext));
      parsed.learning.push_back(ParsedLearning{name, channel.store, std::move(scratch)});
    }
  }
  if (detail::LearningStoreSelected(selected, LearningDataStore::UserDictionary) &&
      stores.user_dictionary) {
    if (const auto* item = FindItem(*items, "user_dictionary")) {
      parsed.user_dictionary =
          std::make_unique<learning::UserDictionary>(std::filesystem::path{}, crypto);
      const bool loaded = parsed.user_dictionary->LoadText(item->plaintext);
      parsed_or_fail(loaded);
      // user-dictionary-pos-spec 3.2: an item of version 3 or later fails the
      // whole import, as a store that blocks saving does.
      if (loaded && parsed.user_dictionary->too_new() && result.error == BackupError::None)
        result.error = BackupError::StoreUnavailable;
    }
  }
  if (detail::LearningStoreSelected(selected, LearningDataStore::Typo) && stores.typo) {
    if (const auto* item = FindItem(*items, "typo_corrections")) {
      parsed.typo =
          std::make_unique<learning::TypoCorrectionStore>(std::filesystem::path{}, crypto);
      parsed_or_fail(parsed.typo->LoadText(item->plaintext));
    }
    if (const auto* item = FindItem(*items, "typo_patterns")) {
      parsed.typo_patterns.emplace();
      parsed_or_fail(
          learning::TypoLearningStore::ParsePatternsText(item->plaintext, *parsed.typo_patterns));
    }
  }
  if (detail::LearningStoreSelected(selected, LearningDataStore::AutoWord) && stores.auto_word) {
    if (const auto* item = FindItem(*items, "auto_words")) {
      parsed.auto_word = std::make_unique<learning::AutoWordStore>(std::filesystem::path{}, crypto);
      parsed_or_fail(parsed.auto_word->LoadText(item->plaintext));
    }
  }
  for (auto& item : *items) learning::SecureErase(item.plaintext);
  if (result.error != BackupError::None) {
    if (parsed.typo_patterns) EraseTypoPatterns(*parsed.typo_patterns);
    return result;
  }

  // The typo store merges in SQLite and can fail without changing anything;
  // it goes first so such a failure leaves the other stores as they were.
  if (parsed.typo_patterns) {
    auto counts = stores.typo->MergePatterns(*parsed.typo_patterns, policy, stores.now_epoch_sec);
    EraseTypoPatterns(*parsed.typo_patterns);
    if (!counts) {
      result.error = BackupError::Io;
      return result;
    }
    result.counts["typo_patterns"] = *counts;
  }
  if (parsed.typo) {
    auto counts = stores.typo->MergeReadingPairs(*parsed.typo, policy);
    if (!counts) {
      result.error = BackupError::Io;
      return result;
    }
    result.counts["typo_corrections"] = *counts;
  }
  for (const auto& item : parsed.learning) {
    result.counts[item.item_name] = item.target->Merge(*item.scratch, policy);
  }
  if (parsed.user_dictionary) {
    result.counts["user_dictionary"] =
        stores.user_dictionary->Merge(*parsed.user_dictionary, policy);
  }
  if (parsed.auto_word) {
    result.counts["auto_words"] = stores.auto_word->Merge(*parsed.auto_word, policy);
  }
  return result;
}

}  // namespace azookey::host
