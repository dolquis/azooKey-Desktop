// M49 learning data management on the engine (DEV-1190,
// docs/learning-data-management-spec.md section 4). Kept apart from
// InferenceEngine.cpp: these settings-app operations touch every store, while
// the conversion path touches them one at a time.

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>

#include "azookey/host/InferenceEngine.h"
#include "azookey/learning/AtomicFile.h"
#include "azookey/learning/FileLock.h"

namespace azookey::host {

namespace {
bool Selected(const std::vector<LearningDataStore>& selected, LearningDataStore store) {
  return std::find(selected.begin(), selected.end(), store) != selected.end();
}

bool UserDictionaryFileExists(const learning::UserDictionary& dict) {
  std::error_code ec;
  return std::filesystem::exists(dict.storage_path(), ec) ||
         std::filesystem::exists(dict.path(), ec);
}
}  // namespace

void InferenceEngine::SetBackupCrypto(const learning::ByteCrypto* crypto) {
  std::scoped_lock lock(state_mutex_);
  backup_crypto_ = crypto;
}

LearningDataStores InferenceEngine::LearningDataStoresLocked() {
  LearningDataStores stores;
  if (store_) stores.learning.push_back({std::string(kKanaLearningChannel), store_});
  if (english_store_) {
    stores.learning.push_back({std::string(kEnglishLearningChannel), english_store_});
  }
  stores.user_dictionary = user_dict_;
  stores.typo = typo_store_;
  stores.auto_word = auto_word_store_;
  return stores;
}

bool InferenceEngine::SaveLearningDataLocked() {
  bool saved = true;
  if (store_ && store_->dirty()) {
    saved = FlushLearningStoreLocked(learning::kTransientFileRetryBudget) && saved;
  }
  saved = SaveEnglishStoreLocked(learning::kTransientFileRetryBudget) && saved;
  return saved;
}

LearningDataPage InferenceEngine::ListLearningEntries(LearningDataStore store,
                                                      const std::string& query, size_t offset,
                                                      size_t limit) {
  std::scoped_lock lock(state_mutex_, english_mutex_, typo_store_mutex_);
  return host::ListLearningEntries(LearningDataStoresLocked(), store, query, offset, limit);
}

InferenceEngine::ForgetOutcome InferenceEngine::ForgetLearningEntry(LearningDataStore store,
                                                                    const std::string& id) {
  std::scoped_lock lock(state_mutex_, english_mutex_, typo_store_mutex_);
  const auto stores = LearningDataStoresLocked();
  const auto entry = FindLearningEntry(stores, store, id);
  if (!entry) return ForgetOutcome::NotFound;

  switch (store) {
    case LearningDataStore::Learning: {
      for (const auto& channel : stores.learning) {
        if (channel.name != entry->channel) continue;
        channel.store->Forget(entry->reading, entry->surface);
        // Forgetting reaches the M7 file too, so an older Host never shows
        // the word again (spec section 4.2).
        const bool legacy_removed =
            channel.store->RemoveFromLegacyFile(entry->reading, entry->surface);
        return SaveLearningDataLocked() && legacy_removed ? ForgetOutcome::Forgotten
                                                          : ForgetOutcome::SaveFailed;
      }
      return ForgetOutcome::NotFound;
    }
    case LearningDataStore::Typo: {
      if (!typo_store_ || !typo_store_->Remove(entry->reading, entry->surface)) {
        return ForgetOutcome::NotFound;
      }
      return typo_store_->Save() ? ForgetOutcome::Forgotten : ForgetOutcome::SaveFailed;
    }
    case LearningDataStore::AutoWord: {
      if (!auto_word_store_) return ForgetOutcome::NotFound;
      const auto previous = auto_word_store_->SetState(entry->surface, entry->reading,
                                                       learning::AutoWordState::Rejected);
      if (!previous) return ForgetOutcome::NotFound;
      if (auto_word_store_->Save()) return ForgetOutcome::Forgotten;
      auto_word_store_->CompareAndSetState(entry->surface, entry->reading,
                                           learning::AutoWordState::Rejected, *previous);
      return ForgetOutcome::SaveFailed;
    }
    case LearningDataStore::UserDictionary:
      break;
  }

  // User dictionary: the same disk-first protocol as RemoveUserWord.
  if (!user_dict_) return ForgetOutcome::NotFound;
  learning::FileLockFailure lock_failure;
  auto file_lock = learning::AcquireExclusiveFileLockForPath(user_dict_->path(), lock_failure);
  if (!file_lock) {
    learning::detail::ReportPersistenceFailure(lock_failure.stage, lock_failure.error);
    return ForgetOutcome::SaveFailed;
  }
  if (UserDictionaryFileExists(*user_dict_) && !user_dict_->Load()) {
    return ForgetOutcome::SaveFailed;
  }
  const auto before = user_dict_->All();
  if (!user_dict_->Remove(entry->surface, entry->reading)) return ForgetOutcome::NotFound;
  if (!user_dict_->Save()) {
    RestoreUserDictionaryLocked(before);
    return ForgetOutcome::SaveFailed;
  }
  RefreshDictionaryLocked();
  return ForgetOutcome::Forgotten;
}

InferenceEngine::ForgetOutcome InferenceEngine::ForgetLearningPair(const std::string& reading,
                                                                   const std::string& surface) {
  std::scoped_lock lock(state_mutex_, english_mutex_, typo_store_mutex_);
  if (!store_ || !store_->Forget(reading, surface)) return ForgetOutcome::NotFound;
  const bool legacy_removed = store_->RemoveFromLegacyFile(reading, surface);
  return SaveLearningDataLocked() && legacy_removed ? ForgetOutcome::Forgotten
                                                    : ForgetOutcome::SaveFailed;
}

LearningExportResult InferenceEngine::ExportLearningData(
    const std::vector<LearningDataStore>& selected, const std::filesystem::path& destination,
    bool encrypt, const BackupManifest& meta) {
  std::scoped_lock lock(state_mutex_, english_mutex_, typo_store_mutex_);
  const auto& crypto = backup_crypto_ ? *backup_crypto_ : learning::DpapiCrypto();
  return host::ExportLearningData(LearningDataStoresLocked(), selected, destination, encrypt,
                                  crypto, meta);
}

LearningImportResult InferenceEngine::ImportLearningData(
    const std::vector<LearningDataStore>& selected, const std::filesystem::path& source,
    learning::ImportConflictPolicy policy) {
  std::scoped_lock lock(state_mutex_, english_mutex_, typo_store_mutex_);
  const auto& crypto = backup_crypto_ ? *backup_crypto_ : learning::DpapiCrypto();
  const bool with_user_dictionary =
      Selected(selected, LearningDataStore::UserDictionary) && user_dict_;

  // The user dictionary is shared with the userdict CLI: take its file lock
  // and start from the disk state, as AddUserWord does.
  std::optional<learning::ScopedFileLock> user_dict_lock;
  std::vector<learning::UserWord> user_dict_before;
  if (with_user_dictionary) {
    learning::FileLockFailure lock_failure;
    user_dict_lock = learning::AcquireExclusiveFileLockForPath(user_dict_->path(), lock_failure);
    if (!user_dict_lock) {
      learning::detail::ReportPersistenceFailure(lock_failure.stage, lock_failure.error);
      LearningImportResult failed;
      failed.error = BackupError::StoreUnavailable;
      return failed;
    }
    if (UserDictionaryFileExists(*user_dict_) && !user_dict_->Load()) {
      LearningImportResult failed;
      failed.error = BackupError::StoreUnavailable;
      return failed;
    }
    user_dict_before = user_dict_->All();
  }

  auto result =
      host::ImportLearningData(LearningDataStoresLocked(), selected, source, policy, crypto);
  if (result.error != BackupError::None) return result;

  bool saved = SaveLearningDataLocked();
  if (Selected(selected, LearningDataStore::Typo) && typo_store_) {
    saved = typo_store_->Save() && saved;
  }
  if (Selected(selected, LearningDataStore::AutoWord) && auto_word_store_) {
    saved = auto_word_store_->Save() && saved;
  }
  if (with_user_dictionary) {
    if (user_dict_->Save()) {
      RefreshDictionaryLocked();
    } else {
      RestoreUserDictionaryLocked(user_dict_before);
      saved = false;
    }
  }
  // The merge stays in memory and is retried by the next flush; report it.
  if (!saved) result.error = BackupError::Io;
  return result;
}

}  // namespace azookey::host
