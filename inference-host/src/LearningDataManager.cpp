#include "azookey/host/LearningDataManager.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "azookey/learning/Sha256.h"

namespace azookey::host {

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
        entries.push_back(std::move(entry));
      }
      break;
    case LearningDataStore::Typo:
      if (!stores.typo) break;
      for (const auto& typo : stores.typo->All()) {
        LearningDataEntry entry;
        entry.id = LearningEntryId(store, "", typo.wrong_reading, typo.correct_reading);
        entry.reading = typo.wrong_reading;
        entry.surface = typo.correct_reading;
        entry.weight = static_cast<double>(typo.record.count);
        entry.last_updated_epoch_sec = typo.record.last_updated_epoch_sec;
        entries.push_back(std::move(entry));
      }
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
    case LearningDataStore::Typo:
      return stores.typo && stores.typo->Remove(entry.reading, entry.surface);
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
  std::unique_ptr<learning::AutoWordStore> auto_word;
};

bool Selected(const std::vector<LearningDataStore>& selected, LearningDataStore store) {
  return std::find(selected.begin(), selected.end(), store) != selected.end();
}

bool AnySelectedStoreBlocked(const LearningDataStores& stores,
                             const std::vector<LearningDataStore>& selected) {
  if (Selected(selected, LearningDataStore::Learning)) {
    for (const auto& channel : stores.learning) {
      if (channel.store && channel.store->save_blocked()) return true;
    }
  }
  return (Selected(selected, LearningDataStore::UserDictionary) && stores.user_dictionary &&
          stores.user_dictionary->save_blocked()) ||
         (Selected(selected, LearningDataStore::Typo) && stores.typo &&
          stores.typo->save_blocked()) ||
         (Selected(selected, LearningDataStore::AutoWord) && stores.auto_word &&
          stores.auto_word->save_blocked());
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

LearningExportResult ExportLearningData(const LearningDataStores& stores,
                                        const std::vector<LearningDataStore>& selected,
                                        const std::filesystem::path& destination, bool encrypt,
                                        const learning::ByteCrypto& crypto,
                                        const BackupManifest& meta) {
  LearningExportResult result;
  if (AnySelectedStoreBlocked(stores, selected)) {
    result.error = BackupError::StoreUnavailable;
    return result;
  }
  std::vector<BackupItem> items;
  if (Selected(selected, LearningDataStore::Learning)) {
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
  if (Selected(selected, LearningDataStore::UserDictionary) && stores.user_dictionary) {
    items.push_back(BackupItem{"user_dictionary", ItemFile("user_dict.json", encrypt),
                               stores.user_dictionary->Size(),
                               stores.user_dictionary->SerializeText()});
  }
  if (Selected(selected, LearningDataStore::Typo) && stores.typo) {
    items.push_back(BackupItem{"typo_corrections", ItemFile("typo_corrections.tsv", encrypt),
                               stores.typo->size(), stores.typo->SerializeText()});
  }
  if (Selected(selected, LearningDataStore::AutoWord) && stores.auto_word) {
    items.push_back(BackupItem{"auto_words", ItemFile("auto_words.tsv", encrypt),
                               stores.auto_word->Size(), stores.auto_word->SerializeText()});
  }

  WriteBackupArchive(destination, items, encrypt, crypto, meta, &result.manifest, &result.file_size,
                     &result.error);
  for (auto& item : items) learning::SecureErase(item.plaintext);
  return result;
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
  // A row that does not parse means the item is not what this Host wrote;
  // importing the rest would lose data silently.
  const auto parsed_or_fail = [&](bool ok) {
    if (!ok) result.error = BackupError::BadItem;
  };

  // Parse everything first: a store is changed only once the whole archive
  // is known to be usable.
  ParsedItems parsed;
  if (Selected(selected, LearningDataStore::Learning)) {
    for (const auto& channel : stores.learning) {
      const auto name = LearningItemName(channel.name);
      const auto* item = channel.store ? FindItem(*items, name) : nullptr;
      if (!item) continue;
      auto scratch = std::make_unique<learning::LearningStore>(std::filesystem::path{}, &crypto);
      parsed_or_fail(scratch->LoadText(item->plaintext));
      parsed.learning.push_back(ParsedLearning{name, channel.store, std::move(scratch)});
    }
  }
  if (Selected(selected, LearningDataStore::UserDictionary) && stores.user_dictionary) {
    if (const auto* item = FindItem(*items, "user_dictionary")) {
      parsed.user_dictionary =
          std::make_unique<learning::UserDictionary>(std::filesystem::path{}, &crypto);
      parsed_or_fail(parsed.user_dictionary->LoadText(item->plaintext));
    }
  }
  if (Selected(selected, LearningDataStore::Typo) && stores.typo) {
    if (const auto* item = FindItem(*items, "typo_corrections")) {
      parsed.typo =
          std::make_unique<learning::TypoCorrectionStore>(std::filesystem::path{}, &crypto);
      parsed_or_fail(parsed.typo->LoadText(item->plaintext));
    }
  }
  if (Selected(selected, LearningDataStore::AutoWord) && stores.auto_word) {
    if (const auto* item = FindItem(*items, "auto_words")) {
      parsed.auto_word =
          std::make_unique<learning::AutoWordStore>(std::filesystem::path{}, &crypto);
      parsed_or_fail(parsed.auto_word->LoadText(item->plaintext));
    }
  }
  for (auto& item : *items) learning::SecureErase(item.plaintext);
  if (result.error != BackupError::None) return result;

  for (const auto& item : parsed.learning) {
    result.counts[item.item_name] = item.target->Merge(*item.scratch, policy);
  }
  if (parsed.user_dictionary) {
    result.counts["user_dictionary"] =
        stores.user_dictionary->Merge(*parsed.user_dictionary, policy);
  }
  if (parsed.typo) result.counts["typo_corrections"] = stores.typo->Merge(*parsed.typo, policy);
  if (parsed.auto_word) {
    result.counts["auto_words"] = stores.auto_word->Merge(*parsed.auto_word, policy);
  }
  return result;
}

}  // namespace azookey::host
