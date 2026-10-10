// M49 learning data management on the engine (DEV-1190,
// docs/learning-data-management-spec.md section 4). Kept apart from
// InferenceEngine.cpp: these settings-app operations touch every store, while
// the conversion path touches them one at a time.

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
  std::lock_guard history_lock(learning_history_mutex_);
  LearningDataStores stores;
  std::optional<LearningDataEntry> entry;
  {
    std::scoped_lock lock(state_mutex_, english_mutex_, typo_store_mutex_);
    stores = LearningDataStoresLocked();
    entry = FindLearningEntry(stores, store, id);
  }
  if (!entry) return ForgetOutcome::NotFound;

  switch (store) {
    case LearningDataStore::Learning: {
      for (const auto& channel : stores.learning) {
        if (channel.name != entry->channel) continue;
        return ForgetPairSnapshot(*channel.store, entry->reading, entry->surface);
      }
      return ForgetOutcome::NotFound;
    }
    case LearningDataStore::Typo: {
      std::optional<learning::TypoCorrectionStore> snapshot;
      {
        std::scoped_lock lock(state_mutex_, typo_store_mutex_);
        if (!typo_store_) return ForgetOutcome::NotFound;
        snapshot = *typo_store_;
      }
      if (!snapshot->Remove(entry->reading, entry->surface)) return ForgetOutcome::NotFound;
      const bool saved = snapshot->Save();
      std::scoped_lock lock(state_mutex_, typo_store_mutex_);
      *typo_store_ = std::move(*snapshot);
      return saved ? ForgetOutcome::Forgotten : ForgetOutcome::SaveFailed;
    }
    case LearningDataStore::AutoWord: {
      if (!auto_word_store_) return ForgetOutcome::NotFound;
      const auto outcome = auto_word_store_->SetStateAndSave(entry->surface, entry->reading,
                                                             learning::AutoWordState::Rejected);
      if (outcome == learning::AutoWordSaveOutcome::NotFound) return ForgetOutcome::NotFound;
      return outcome == learning::AutoWordSaveOutcome::SaveFailed ? ForgetOutcome::SaveFailed
                                                                  : ForgetOutcome::Forgotten;
    }
    case LearningDataStore::UserDictionary:
      break;
  }

  // Keep the live dictionary available while the snapshot follows the same
  // disk-first/file-lock protocol as RemoveUserWord.
  if (!user_dict_) return ForgetOutcome::NotFound;
  std::optional<learning::UserDictionary> snapshot;
  {
    std::scoped_lock lock(state_mutex_);
    snapshot = *user_dict_;
  }
  learning::FileLockFailure lock_failure;
  auto file_lock = learning::AcquireExclusiveFileLockForPath(snapshot->path(), lock_failure);
  if (!file_lock) {
    learning::detail::ReportPersistenceFailure(lock_failure.stage, lock_failure.error);
    return ForgetOutcome::SaveFailed;
  }
  if (UserDictionaryFileExists(*snapshot) && !snapshot->Load()) {
    return ForgetOutcome::SaveFailed;
  }
  const auto before = *snapshot;
  if (!snapshot->Remove(entry->surface, entry->reading)) return ForgetOutcome::NotFound;
  const bool saved = snapshot->Save();
  if (!saved) *snapshot = before;
  std::scoped_lock lock(state_mutex_);
  *user_dict_ = std::move(*snapshot);
  indexed_user_dict_ = nullptr;
  RefreshDictionaryLocked();
  return saved ? ForgetOutcome::Forgotten : ForgetOutcome::SaveFailed;
}

InferenceEngine::ForgetOutcome InferenceEngine::ForgetLearningPair(const std::string& reading,
                                                                   const std::string& surface) {
  std::lock_guard history_lock(learning_history_mutex_);
  if (!store_) return ForgetOutcome::NotFound;
  return ForgetPairSnapshot(*store_, reading, surface);
}

InferenceEngine::LearningSnapshots InferenceEngine::SnapshotLearningStoresLocked() {
  LearningSnapshots snapshots;
  for (auto* store : {store_, english_store_}) {
    if (store) snapshots.emplace_back(store, *store);
  }
  learning_erasure_in_progress_ = true;
  learning_flush_cv_.notify_all();
  return snapshots;
}

bool InferenceEngine::SaveLearningSnapshots(LearningSnapshots& snapshots) {
  bool saved = true;
  for (auto& [target, snapshot] : snapshots) {
    if (!snapshot.dirty() || snapshot.Save(learning::kTransientFileRetryBudget)) continue;
    saved = false;
    if (target == store_) {
      std::scoped_lock lock(state_mutex_);
      RecordLearningSaveFailureLocked();
      first_unsaved_observation_steady_ = std::chrono::steady_clock::now();
    } else if (runtime_logger_) {
      runtime_logger_->Log(logging::RuntimeLogLevel::Warn, "english_learning_save_failed", {});
    }
  }
  return saved;
}

void InferenceEngine::PublishLearningSnapshotsLocked(LearningSnapshots& snapshots) {
  for (auto& [target, snapshot] : snapshots) *target = std::move(snapshot);
  learning_erasure_in_progress_ = false;
  // A successful snapshot save already cleared dirty: this only clears the
  // burst counters, and performs no I/O. A failed save retains its retry state.
  if (store_ && !store_->dirty()) (void)FlushLearningStoreLocked(std::chrono::milliseconds::zero());
  learning_flush_cv_.notify_all();
}

// Forgets the pair in memory (dropped from the v2 file by the save) and in the
// M7 file. The M7 removal is attempted even when the v2 store has no such pair:
// a pair learned only before v2, or one whose earlier M7 removal failed, is
// still there, and a retry must be able to finish it.
InferenceEngine::ForgetOutcome InferenceEngine::ForgetPairSnapshot(learning::LearningStore& store,
                                                                   const std::string& reading,
                                                                   const std::string& surface) {
  LearningSnapshots snapshots;
  learning::LearningStore* selected = nullptr;
  bool in_store = false;
  {
    std::scoped_lock lock(state_mutex_, english_mutex_);
    snapshots = SnapshotLearningStoresLocked();
    for (auto& [target, snapshot] : snapshots) {
      if (target == &store) {
        in_store = snapshot.Forget(reading, surface);
        selected = &snapshot;
      }
      if (target == store_ && snapshot.dirty() &&
          (last_unsaved_observation_epoch_sec_ || first_unsaved_observation_epoch_sec_)) {
        snapshot.Prune(config_.learning_max_records, config_.learning_min_weight,
                       last_unsaved_observation_epoch_sec_.value_or(
                           first_unsaved_observation_epoch_sec_.value_or(0)));
      }
    }
  }
  bool legacy_removed = false;
  bool legacy_ok = false;
  bool saved = false;
  try {
    legacy_ok = selected && selected->RemoveFromLegacyFile(reading, surface, &legacy_removed);
    saved = SaveLearningSnapshots(snapshots);
  } catch (...) {
    std::scoped_lock lock(state_mutex_, english_mutex_);
    learning_erasure_in_progress_ = false;
    learning_flush_cv_.notify_all();
    throw;
  }
  // Queries read the live stores during persistence. Publish memory-first
  // forgetting even on an I/O failure, atomically with converter erasure.
  std::scoped_lock lock(state_mutex_, english_mutex_, converter_call_mutex_);
  PublishLearningSnapshotsLocked(snapshots);
  if (&store == store_) {
    fallback_converter_->Forget(reading, surface);
    if (model_converter_) model_converter_->Forget(reading, surface);
  }
  if (in_store && &store == store_) RefreshPersonaLocked();
  if (!legacy_ok || !saved) return ForgetOutcome::SaveFailed;
  return in_store || legacy_removed ? ForgetOutcome::Forgotten : ForgetOutcome::NotFound;
}

bool InferenceEngine::IngestTrendingWords(const std::vector<learning::AutoWord>& words,
                                          uint64_t now_epoch, bool auto_promote) {
  std::lock_guard history_lock(learning_history_mutex_);
  if (!auto_word_store_ || auto_word_store_->save_blocked()) return false;
  auto_word_store_->IngestTrending(words, now_epoch, auto_promote);
  return auto_word_store_->Save();
}

InferenceEngine::ResetOutcome InferenceEngine::ResetLearningStore(LearningDataStore store) {
  std::lock_guard history_lock(learning_history_mutex_);
  if (store == LearningDataStore::Learning) return ResetLearningChannels();
  switch (store) {
    case LearningDataStore::Learning:
      break;  // Handled above, including the converter history reset.
    case LearningDataStore::Typo: {
      std::optional<learning::TypoCorrectionStore> snapshot;
      {
        std::scoped_lock lock(state_mutex_, typo_store_mutex_);
        if (!typo_store_) return ResetOutcome::Unavailable;
        if (typo_store_->save_blocked()) return ResetOutcome::SaveFailed;
        snapshot = *typo_store_;
      }
      snapshot->Reset();
      if (!snapshot->Save()) return ResetOutcome::SaveFailed;
      std::scoped_lock lock(state_mutex_, typo_store_mutex_);
      *typo_store_ = std::move(*snapshot);
      return ResetOutcome::Reset;
    }
    case LearningDataStore::AutoWord: {
      if (!auto_word_store_) return ResetOutcome::Unavailable;
      return auto_word_store_->ResetAndSave() ? ResetOutcome::Reset : ResetOutcome::SaveFailed;
    }
    case LearningDataStore::UserDictionary:
      break;
  }

  // User dictionary: the same disk-first protocol as RemoveUserWord.
  if (!user_dict_) return ResetOutcome::Unavailable;
  std::optional<learning::UserDictionary> snapshot;
  {
    std::scoped_lock lock(state_mutex_);
    snapshot = *user_dict_;
  }
  learning::FileLockFailure lock_failure;
  auto file_lock = learning::AcquireExclusiveFileLockForPath(snapshot->path(), lock_failure);
  if (!file_lock) {
    learning::detail::ReportPersistenceFailure(lock_failure.stage, lock_failure.error);
    return ResetOutcome::SaveFailed;
  }
  if (UserDictionaryFileExists(*snapshot) && !snapshot->Load()) {
    return ResetOutcome::SaveFailed;
  }
  const auto before = *snapshot;
  snapshot->Clear();
  const bool saved = snapshot->Save();
  if (!saved) *snapshot = before;
  std::scoped_lock lock(state_mutex_);
  *user_dict_ = std::move(*snapshot);
  indexed_user_dict_ = nullptr;
  RefreshDictionaryLocked();
  return saved ? ResetOutcome::Reset : ResetOutcome::SaveFailed;
}

// Every learning channel is emptied or none is. A store whose file could not
// be read refuses to save, so its file would survive an "ok"; it is refused
// before anything changes. The v2 files are emptied before the M7 file, so the
// v2 file exists (empty) before its M7 source is touched. A failure at either
// step puts the snapshot back in memory and saves it again.
InferenceEngine::ResetOutcome InferenceEngine::ResetLearningChannels() {
  LearningSnapshots snapshots;
  {
    std::scoped_lock lock(state_mutex_, english_mutex_);
    for (auto* store : {store_, english_store_}) {
      if (store && store->save_blocked()) return ResetOutcome::SaveFailed;
    }
    if (!store_ && !english_store_) return ResetOutcome::Unavailable;
    snapshots = SnapshotLearningStoresLocked();
  }
  const auto before = snapshots;
  bool saved = false;
  try {
    for (auto& [target, snapshot] : snapshots) snapshot.Reset();
    saved = SaveLearningSnapshots(snapshots) && (!store_ || store_->ClearLegacyFile());
    if (!saved) {
      for (size_t i = 0; i < snapshots.size(); ++i)
        snapshots[i].second.LoadText(before[i].second.SerializeText());
      // A channel whose empty snapshot reached disk gets its old rows back;
      // a failed rollback remains dirty for the next flush.
      (void)SaveLearningSnapshots(snapshots);
    }
  } catch (...) {
    std::scoped_lock lock(state_mutex_, english_mutex_);
    learning_erasure_in_progress_ = false;
    learning_flush_cv_.notify_all();
    throw;
  }
  std::scoped_lock lock(state_mutex_, english_mutex_, converter_call_mutex_);
  PublishLearningSnapshotsLocked(snapshots);
  if (saved) {
    fallback_converter_->ResetLearned();
    if (model_converter_) model_converter_->ResetLearned();
    RefreshPersonaLocked();
  }
  return saved ? ResetOutcome::Reset : ResetOutcome::SaveFailed;
}

std::optional<InferenceEngine::PersonaSnapshot> InferenceEngine::CurrentPersona() {
  std::scoped_lock lock(state_mutex_);
  if (!store_) return std::nullopt;
  if (!persona_) RefreshPersonaLocked();
  return persona_;
}

void InferenceEngine::RefreshPersona() {
  std::scoped_lock lock(state_mutex_);
  RefreshPersonaLocked();
}

void InferenceEngine::RefreshPersonaLocked() {
  if (!store_) return;
  PersonaSnapshot snapshot;
  snapshot.persona = learning::ComputePersona(store_->Aggregates());
  snapshot.computed_at_epoch_sec =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
  persona_ = snapshot;
  persona_computed_steady_ = std::chrono::steady_clock::now();
}

LearningExportResult InferenceEngine::ExportLearningData(
    const std::vector<LearningDataStore>& selected, const std::filesystem::path& destination,
    bool encrypt, const BackupManifest& meta) {
  // Snapshot under the store locks; encrypt, zip and write outside them so the
  // conversion path is not stalled by the export.
  std::optional<std::vector<BackupItem>> items;
  const learning::ByteCrypto* crypto = nullptr;
  LearningExportResult result;
  {
    std::scoped_lock lock(state_mutex_, english_mutex_, typo_store_mutex_);
    crypto = backup_crypto_ ? backup_crypto_ : &learning::DpapiCrypto();
    items = CollectBackupItems(LearningDataStoresLocked(), selected, encrypt, &result.error);
  }
  if (!items) return result;
  return WriteLearningBackup(std::move(*items), destination, encrypt, *crypto, meta);
}

LearningImportResult InferenceEngine::ImportLearningData(
    const std::vector<LearningDataStore>& selected, const std::filesystem::path& source,
    learning::ImportConflictPolicy policy) {
  std::lock_guard history_lock(learning_history_mutex_);
  // Read, verify and decrypt the archive outside the store locks; only the
  // parse and merge below hold them.
  const learning::ByteCrypto* crypto = nullptr;
  {
    std::scoped_lock lock(state_mutex_);
    crypto = backup_crypto_ ? backup_crypto_ : &learning::DpapiCrypto();
  }
  LearningImportResult read_failure;
  BackupManifest manifest;
  auto items = ReadBackupArchive(source, *crypto, &manifest, &read_failure.error);
  if (!items) return read_failure;

  std::scoped_lock lock(state_mutex_, english_mutex_, typo_store_mutex_);
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

  auto result = ApplyImportedItems(LearningDataStoresLocked(), selected, std::move(*items), policy);
  if (result.error != BackupError::None) return result;
  if (Selected(selected, LearningDataStore::Learning)) RefreshPersonaLocked();

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
  // A store that failed to save keeps the merge in memory and retries it at
  // the next flush (the user dictionary is rolled back above); the response
  // says "io" so the settings app reports that the import is not on disk yet
  // (learning-data-management-spec section 4.5).
  if (!saved) result.error = BackupError::Io;
  return result;
}

}  // namespace azookey::host
