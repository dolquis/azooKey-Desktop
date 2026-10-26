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
  std::unique_lock history_lock(learning_history_mutex_);
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
      if (outcome == learning::AutoWordSaveOutcome::SaveFailed) return ForgetOutcome::SaveFailed;
      // DEV-1596: a word the English dictionary gained from its confirmation
      // leaves it, outside the history lock as every English write does. The
      // response does not depend on it; the engine logs the result.
      if (outcome != learning::AutoWordSaveOutcome::Unchanged) {
        history_lock.unlock();
        RetractRejectedEnglishWord(entry->surface, "forget");
      }
      return ForgetOutcome::Forgotten;
    }
    case LearningDataStore::UserDictionary:
      break;
  }

  // Keep the live dictionary available while the snapshot follows the same
  // disk-first/file-lock protocol as RemoveUserWord.
  if (!user_dict_) return ForgetOutcome::NotFound;
  std::lock_guard writer_lock(user_dict_writer_mutex_);
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
  if (detail::UserDictionaryFileExists(*snapshot) && !snapshot->ReloadIfChanged()) {
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
  // A forget or a reset may have removed M63-B negative observations.
  ClearEnglishSuppressedCache();
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
  return IngestTrendingWords(words, now_epoch, auto_promote, {}) == TrendingIngestOutcome::Saved;
}

InferenceEngine::TrendingIngestOutcome InferenceEngine::IngestTrendingWords(
    const std::vector<learning::AutoWord>& words, uint64_t now_epoch, bool auto_promote,
    const std::function<bool()>& still_current) {
  std::vector<std::string> newly_confirmed;
  uint64_t english_epoch = 0;
  {
    std::lock_guard history_lock(learning_history_mutex_);
    if (still_current && !still_current()) return TrendingIngestOutcome::Cancelled;
    // Under the history lock, before the save: an AutoWord reset that follows
    // cancels the upserts below (DEV-1600).
    english_epoch = english_registration_epoch_.load();
    if (!auto_word_store_ || auto_word_store_->save_blocked()) {
      return TrendingIngestOutcome::SaveFailed;
    }
    // DEV-1584: which words of the batch this ingest promotes to Confirmed.
    // Only ASCII words can enter the English dictionary; the rest are not
    // tracked.
    const auto is_confirmed = [&](const learning::AutoWord& word) {
      const auto confirmed = auto_word_store_->LookupConfirmed(word.reading);
      return std::any_of(confirmed.begin(), confirmed.end(),
                         [&](const auto& c) { return c.surface == word.surface; });
    };
    std::vector<const learning::AutoWord*> unconfirmed;
    if (auto_promote) {
      for (const auto& word : words) {
        const bool ascii = !word.surface.empty() &&
                           std::all_of(word.surface.begin(), word.surface.end(),
                                       [](char c) { return static_cast<unsigned char>(c) < 0x80; });
        if (ascii && !is_confirmed(word)) unconfirmed.push_back(&word);
      }
    }
    auto_word_store_->IngestTrending(words, now_epoch, auto_promote);
    if (!auto_word_store_->Save()) return TrendingIngestOutcome::SaveFailed;
    for (const auto* word : unconfirmed) {
      if (is_confirmed(*word)) newly_confirmed.push_back(word->surface);
    }
  }
  // Outside the history lock: each upsert may wait for the English
  // dictionary's writer lock and flush two files, and commits must not queue
  // behind a whole batch of them.
  for (const auto& surface : newly_confirmed) {
    RegisterConfirmedEnglishWord(surface, "trending", english_epoch);
  }
  return TrendingIngestOutcome::Saved;
}

InferenceEngine::ResetOutcome InferenceEngine::ResetLearningStore(LearningDataStore store) {
  std::unique_lock history_lock(learning_history_mutex_);
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
      if (!auto_word_store_->ResetAndSave()) return ResetOutcome::SaveFailed;
      // DEV-1600: the English dictionary's runtime additions derive from the
      // rows just erased. Raised after the save, so an upsert for a word
      // confirmed before the reset is cancelled (RegisterConfirmedEnglishWord).
      english_registration_epoch_.fetch_add(1);
      // Outside the history lock, as every English write is: the writer lock
      // wait and a TSV recompile must not hold commits back. A failure leaves
      // the AutoWord store empty and is reported; a retry erases the same
      // words, as the journal survives every failed step.
      history_lock.unlock();
      return ResetEnglishRuntimeAdditions() ? ResetOutcome::Reset : ResetOutcome::SaveFailed;
    }
    case LearningDataStore::UserDictionary:
      break;
  }

  // User dictionary: the same disk-first protocol as RemoveUserWord.
  if (!user_dict_) return ResetOutcome::Unavailable;
  std::lock_guard writer_lock(user_dict_writer_mutex_);
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
  if (detail::UserDictionaryFileExists(*snapshot) && !snapshot->ReloadIfChanged()) {
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

  // learning_history_mutex_ keeps user_dict_ (the pointer) in place.
  const bool with_user_dictionary =
      detail::LearningStoreSelected(selected, LearningDataStore::UserDictionary) && user_dict_;

  // The user dictionary is shared with the userdict CLI: take its file lock
  // and start from the disk state, as AddUserWord does. The merge goes into a
  // copy, and the wait for the file lock and the copy's load and save happen
  // outside state_mutex_, so queries are not held behind them (DEV-1572).
  std::unique_lock<std::timed_mutex> user_dict_writer(user_dict_writer_mutex_, std::defer_lock);
  std::optional<learning::ScopedFileLock> user_dict_lock;
  std::optional<learning::UserDictionary> user_dict_copy;
  if (with_user_dictionary) {
    user_dict_writer.lock();
    {
      std::scoped_lock lock(state_mutex_);
      user_dict_copy = *user_dict_;
    }
    if (before_user_dictionary_file_lock_for_tests_) before_user_dictionary_file_lock_for_tests_();
    learning::FileLockFailure lock_failure;
    user_dict_lock =
        learning::AcquireExclusiveFileLockForPath(user_dict_copy->path(), lock_failure);
    if (!user_dict_lock) {
      learning::detail::ReportPersistenceFailure(lock_failure.stage, lock_failure.error);
      LearningImportResult failed;
      failed.error = BackupError::StoreUnavailable;
      return failed;
    }
    if (detail::UserDictionaryFileExists(*user_dict_copy) && !user_dict_copy->ReloadIfChanged()) {
      LearningImportResult failed;
      failed.error = BackupError::StoreUnavailable;
      return failed;
    }
  }

  LearningImportResult result;
  bool saved = true;
  {
    std::scoped_lock lock(state_mutex_, english_mutex_, typo_store_mutex_);
    auto stores = LearningDataStoresLocked();
    if (user_dict_copy) stores.user_dictionary = &*user_dict_copy;
    result = ApplyImportedItems(stores, selected, std::move(*items), policy);
    // Before any return: a failed item may have left others applied.
    ClearEnglishSuppressedCache();
    if (result.error != BackupError::None) return result;
    if (detail::LearningStoreSelected(selected, LearningDataStore::Learning)) {
      RefreshPersonaLocked();
    }

    saved = SaveLearningDataLocked();
    if (detail::LearningStoreSelected(selected, LearningDataStore::Typo) && typo_store_) {
      saved = typo_store_->Save() && saved;
    }
    if (detail::LearningStoreSelected(selected, LearningDataStore::AutoWord) && auto_word_store_) {
      saved = auto_word_store_->Save() && saved;
    }
  }
  if (user_dict_copy) {
    // Published only once on disk; a failed save leaves the live dictionary
    // as it was before the import.
    if (user_dict_copy->Save()) {
      std::scoped_lock lock(state_mutex_);
      *user_dict_ = std::move(*user_dict_copy);
      indexed_user_dict_ = nullptr;
      RefreshDictionaryLocked();
    } else {
      saved = false;
    }
  }
  // A store that failed to save keeps the merge in memory and retries it at
  // the next flush (the user dictionary keeps its state before the import);
  // the response says "io" so the settings app reports that the import is not
  // on disk yet (learning-data-management-spec section 4.5).
  if (!saved) result.error = BackupError::Io;
  return result;
}

}  // namespace azookey::host
