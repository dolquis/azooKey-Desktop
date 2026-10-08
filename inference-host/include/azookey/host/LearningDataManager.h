#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/host/BackupArchive.h"
#include "azookey/learning/AutoWordStore.h"
#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/ImportConflictPolicy.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/TypoCorrectionStore.h"
#include "azookey/learning/UserDictionary.h"

namespace azookey::host {

// learning-data-management-spec section 4: the stores the settings app can
// list, forget, export and import through the Host.
enum class LearningDataStore : uint8_t { Learning, UserDictionary, Typo, AutoWord };

// IPC names: "learning", "user_dict", "typo", "auto_word".
std::string_view LearningDataStoreName(LearningDataStore store);
std::optional<LearningDataStore> ParseLearningDataStore(std::string_view name);

inline constexpr size_t kMaxLearningListLimit = 500;
// Learning channels (DEV-1196 keeps English learning in its own file).
inline constexpr std::string_view kKanaLearningChannel = "kana";
inline constexpr std::string_view kEnglishLearningChannel = "english";

struct LearningDataEntry {
  // Stable across restarts: derived from the store, channel and key (no
  // counter to persist).
  std::string id;
  // Learning channel; empty for the other stores.
  std::string channel;
  std::string reading;
  std::string surface;
  double weight{};
  uint64_t last_updated_epoch_sec{};
  std::vector<std::string> tags;
  std::map<std::string, std::string> metadata;
};

struct LearningDataPage {
  size_t total{};
  std::vector<LearningDataEntry> entries;
};

struct LearningChannel {
  std::string name;
  learning::LearningStore* store{};
};

// Borrowed stores; a null pointer means the store is unavailable. The caller
// serializes access (the stores other than AutoWordStore are not
// thread-safe) and saves the stores after Forget or Import.
struct LearningDataStores {
  std::vector<LearningChannel> learning;
  learning::UserDictionary* user_dictionary{};
  learning::TypoCorrectionStore* typo{};
  learning::AutoWordStore* auto_word{};
};

std::string LearningEntryId(LearningDataStore store, std::string_view channel,
                            std::string_view reading, std::string_view surface);

// Entries whose reading or surface contains `query` (empty matches all),
// ordered by channel, reading, surface. `limit` is clamped to
// kMaxLearningListLimit; `total` counts every match.
LearningDataPage ListLearningEntries(const LearningDataStores& stores, LearningDataStore store,
                                     std::string_view query, size_t offset, size_t limit);

// The entry with this id, whatever the page size; nullopt when none matches.
std::optional<LearningDataEntry> FindLearningEntry(const LearningDataStores& stores,
                                                   LearningDataStore store, std::string_view id);

// Per store (spec section 4.2): learning zeroes the pair (dropped on the next
// save), typo and user_dict remove the entry, auto_word marks it rejected so it
// is not proposed again. Returns false when nothing matched.
bool ForgetLearningEntry(const LearningDataStores& stores, LearningDataStore store,
                         std::string_view id);
// The TIP's Ctrl+Shift+Backspace form: the most recent commit's pair in the
// kana learning channel, by key instead of id.
bool ForgetLearningPair(const LearningDataStores& stores, const std::string& reading,
                        const std::string& surface);

struct LearningExportResult {
  BackupError error{BackupError::None};
  uint64_t file_size{};
  BackupManifest manifest;
};

LearningExportResult ExportLearningData(const LearningDataStores& stores,
                                        const std::vector<LearningDataStore>& selected,
                                        const std::filesystem::path& destination, bool encrypt,
                                        const learning::ByteCrypto& crypto,
                                        const BackupManifest& meta);

struct LearningImportResult {
  BackupError error{BackupError::None};
  // Keyed by archive item name ("learning", "user_dictionary", ...).
  std::map<std::string, learning::ImportCounts> counts;
};

// The two halves of ExportLearningData, so a caller can take the snapshot under
// its store locks and write the archive (encryption, ZIP, disk) outside them.
std::optional<std::vector<BackupItem>> CollectBackupItems(
    const LearningDataStores& stores, const std::vector<LearningDataStore>& selected, bool encrypt,
    BackupError* error);
LearningExportResult WriteLearningBackup(std::vector<BackupItem> items,
                                         const std::filesystem::path& destination, bool encrypt,
                                         const learning::ByteCrypto& crypto,
                                         const BackupManifest& meta);

// The store half of ImportLearningData: parses the decrypted items, then merges
// them only when all parse. ReadBackupArchive can run outside the locks.
LearningImportResult ApplyImportedItems(const LearningDataStores& stores,
                                        const std::vector<LearningDataStore>& selected,
                                        std::vector<BackupItem> items,
                                        learning::ImportConflictPolicy policy);

// Reads and parses every selected item before changing any store, so a bad
// archive leaves the stores untouched. A selected store absent from the
// archive is skipped.
LearningImportResult ImportLearningData(const LearningDataStores& stores,
                                        const std::vector<LearningDataStore>& selected,
                                        const std::filesystem::path& source,
                                        learning::ImportConflictPolicy policy,
                                        const learning::ByteCrypto& crypto);

}  // namespace azookey::host
