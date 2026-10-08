#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "azookey/host/LearningDataManager.h"

namespace {

namespace host = azookey::host;
namespace learning = azookey::learning;
using host::LearningDataStore;

constexpr uint64_t kNow = 2'000'000'000;

// Two mock keys standing in for two Windows accounts: data encrypted with one
// cannot be decrypted with the other.
class XorCrypto final : public learning::ByteCrypto {
 public:
  explicit XorCrypto(uint8_t key) : key_(key) {}
  bool Encrypt(const std::vector<uint8_t>& plain, std::vector<uint8_t>& cipher) const override {
    cipher = {key_};
    for (const uint8_t byte : plain) cipher.push_back(byte ^ key_);
    return true;
  }
  bool Decrypt(const std::vector<uint8_t>& cipher, std::vector<uint8_t>& plain) const override {
    if (cipher.empty() || cipher.front() != key_) return false;
    plain.assign(cipher.begin() + 1, cipher.end());
    for (auto& byte : plain) byte ^= key_;
    return true;
  }

 private:
  uint8_t key_;
};

const XorCrypto kKeyA(0x3C);
const XorCrypto kKeyB(0xC3);

class ScopedDirectory {
 public:
  ScopedDirectory() {
    static std::atomic<uint64_t> next{0};
    path_ = std::filesystem::temp_directory_path() /
            ("azookey_learning_data_manager_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
             std::to_string(next++));
    std::filesystem::create_directories(path_);
  }
  ~ScopedDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  ScopedDirectory(const ScopedDirectory&) = delete;
  ScopedDirectory& operator=(const ScopedDirectory&) = delete;

  std::filesystem::path File(const char* name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

// In-memory stores; nothing here is saved to disk.
struct Stores {
  learning::LearningStore kana{"kana.tsv", &kKeyA};
  learning::LearningStore english{"english.tsv", &kKeyA};
  learning::UserDictionary user_dictionary{"user_dict.json", &kKeyA};
  learning::TypoCorrectionStore typo{"typo.tsv", &kKeyA};
  learning::AutoWordStore auto_word{"auto_words.tsv", &kKeyA};

  host::LearningDataStores View() {
    return host::LearningDataStores{{{std::string(host::kKanaLearningChannel), &kana},
                                     {std::string(host::kEnglishLearningChannel), &english}},
                                    &user_dictionary,
                                    &typo,
                                    &auto_word};
  }

  void Fill() {
    kana.Observe("にほん", "日本", 0.8, kNow);
    kana.Observe("にほんご", "日本語", 1.6, kNow);
    kana.ObserveEvent({"にほんご", "日本語", "code.exe", learning::LearningEventType::Commit, ""},
                      0.8, kNow);
    english.Observe("helo", "hello", 0.8, kNow);
    user_dictionary.Add({"東京特許", "とうきょうとっきょ", std::nullopt, std::nullopt, 1.5});
    typo.Observe("こんちには", "こんにちは", kNow);
    auto_word.Observe("推し活", "おしかつ", kNow, 3, false);
  }

  std::vector<std::string> Texts() const {
    return {kana.SerializeText(), english.SerializeText(), user_dictionary.SerializeText(),
            typo.SerializeText(), auto_word.SerializeText()};
  }
};

const std::vector<LearningDataStore> kAllStores = {
    LearningDataStore::Learning, LearningDataStore::UserDictionary, LearningDataStore::Typo,
    LearningDataStore::AutoWord};

}  // namespace

TEST(LearningDataManagerTest, StoreNamesRoundTrip) {
  for (const auto store : kAllStores) {
    EXPECT_EQ(host::ParseLearningDataStore(host::LearningDataStoreName(store)), store);
  }
  EXPECT_FALSE(host::ParseLearningDataStore("dictionary").has_value());
}

TEST(LearningDataManagerTest, ListsLearningPairsAcrossChannelsWithPaging) {
  Stores stores;
  stores.Fill();
  const auto view = stores.View();

  const auto all = host::ListLearningEntries(view, LearningDataStore::Learning, "", 0, 100);
  ASSERT_EQ(all.total, 3u);
  ASSERT_EQ(all.entries.size(), 3u);
  EXPECT_EQ(all.entries[0].channel, "kana");
  EXPECT_EQ(all.entries[1].surface, "日本語");
  // Two app rows are one listed pair with the summed weight.
  EXPECT_DOUBLE_EQ(all.entries[1].weight, 2.4);
  EXPECT_EQ(all.entries[1].tags, std::vector<std::string>{"code.exe"});
  EXPECT_EQ(all.entries[1].metadata.at("commit_count"), "2");
  EXPECT_EQ(all.entries[2].channel, "english");

  const auto page = host::ListLearningEntries(view, LearningDataStore::Learning, "", 1, 1);
  EXPECT_EQ(page.total, 3u);
  ASSERT_EQ(page.entries.size(), 1u);
  EXPECT_EQ(page.entries[0].id, all.entries[1].id);

  const auto filtered =
      host::ListLearningEntries(view, LearningDataStore::Learning, "日本語", 0, 10);
  EXPECT_EQ(filtered.total, 1u);
  const auto by_reading =
      host::ListLearningEntries(view, LearningDataStore::Learning, "にほん", 0, 0);
  EXPECT_EQ(by_reading.total, 2u);
  EXPECT_TRUE(by_reading.entries.empty());
}

TEST(LearningDataManagerTest, ListsEveryStoreKind) {
  Stores stores;
  stores.Fill();
  const auto view = stores.View();
  const auto dictionary =
      host::ListLearningEntries(view, LearningDataStore::UserDictionary, "", 0, 10);
  ASSERT_EQ(dictionary.entries.size(), 1u);
  EXPECT_EQ(dictionary.entries[0].reading, "とうきょうとっきょ");
  EXPECT_DOUBLE_EQ(dictionary.entries[0].weight, 1.5);
  const auto typo = host::ListLearningEntries(view, LearningDataStore::Typo, "", 0, 10);
  ASSERT_EQ(typo.entries.size(), 1u);
  EXPECT_EQ(typo.entries[0].surface, "こんにちは");
  const auto words = host::ListLearningEntries(view, LearningDataStore::AutoWord, "", 0, 10);
  ASSERT_EQ(words.entries.size(), 1u);
  EXPECT_EQ(words.entries[0].tags.front(), "pending");
}

TEST(LearningDataManagerTest, LimitIsClampedToTheMaximum) {
  Stores stores;
  for (size_t i = 0; i < host::kMaxLearningListLimit + 5; ++i) {
    stores.kana.Observe("よみ" + std::to_string(i), "表記", 0.8, kNow);
  }
  const auto page =
      host::ListLearningEntries(stores.View(), LearningDataStore::Learning, "", 0, 100000);
  EXPECT_EQ(page.total, host::kMaxLearningListLimit + 5);
  EXPECT_EQ(page.entries.size(), host::kMaxLearningListLimit);
}

TEST(LearningDataManagerTest, EntryIdsAreStableAndScopedByStoreAndChannel) {
  const auto id = host::LearningEntryId(LearningDataStore::Learning, "kana", "にほん", "日本");
  EXPECT_EQ(id.size(), 16u);
  EXPECT_EQ(id, host::LearningEntryId(LearningDataStore::Learning, "kana", "にほん", "日本"));
  EXPECT_NE(id, host::LearningEntryId(LearningDataStore::Learning, "english", "にほん", "日本"));
  EXPECT_NE(id, host::LearningEntryId(LearningDataStore::Typo, "kana", "にほん", "日本"));
  // Field boundaries cannot be shifted to forge another key.
  EXPECT_NE(host::LearningEntryId(LearningDataStore::Typo, "", "ab", "c"),
            host::LearningEntryId(LearningDataStore::Typo, "", "a", "bc"));
}

TEST(LearningDataManagerTest, ForgetFollowsEachStoreRule) {
  Stores stores;
  stores.Fill();
  const auto view = stores.View();
  const auto first = [&](LearningDataStore store) {
    return host::ListLearningEntries(view, store, "", 0, 10).entries.front().id;
  };

  EXPECT_TRUE(host::ForgetLearningEntry(view, LearningDataStore::Learning,
                                        first(LearningDataStore::Learning)));
  EXPECT_DOUBLE_EQ(stores.kana.Score("にほん", "日本", kNow), 0.0);
  EXPECT_EQ(host::ListLearningEntries(view, LearningDataStore::Learning, "", 0, 10).total, 2u);

  EXPECT_TRUE(host::ForgetLearningEntry(view, LearningDataStore::UserDictionary,
                                        first(LearningDataStore::UserDictionary)));
  EXPECT_EQ(stores.user_dictionary.Size(), 0u);
  EXPECT_TRUE(
      host::ForgetLearningEntry(view, LearningDataStore::Typo, first(LearningDataStore::Typo)));
  EXPECT_EQ(stores.typo.size(), 0u);
  EXPECT_TRUE(host::ForgetLearningEntry(view, LearningDataStore::AutoWord,
                                        first(LearningDataStore::AutoWord)));
  // A rejected word stays recorded so that it is not proposed again.
  EXPECT_EQ(stores.auto_word.ListByState(learning::AutoWordState::Rejected).size(), 1u);

  EXPECT_FALSE(host::ForgetLearningEntry(view, LearningDataStore::Learning, "0000000000000000"));
}

TEST(LearningDataManagerTest, ForgetLearningPairTargetsTheKanaChannel) {
  Stores stores;
  stores.Fill();
  EXPECT_TRUE(host::ForgetLearningPair(stores.View(), "にほんご", "日本語"));
  EXPECT_DOUBLE_EQ(stores.kana.Score("にほんご", "日本語", kNow), 0.0);
  EXPECT_FALSE(host::ForgetLearningPair(stores.View(), "helo", "hello"));
  EXPECT_GT(stores.english.Score("helo", "hello", kNow), 0.0);
}

TEST(LearningDataManagerTest, ExportImportRoundTripRestoresEveryStore) {
  ScopedDirectory directory;
  const auto archive = directory.File("backup.zip");
  Stores source;
  source.Fill();
  const auto exported = host::ExportLearningData(source.View(), kAllStores, archive, true, kKeyA,
                                                 host::BackupManifest{});
  ASSERT_EQ(exported.error, host::BackupError::None);
  EXPECT_GT(exported.file_size, 0u);
  // Two learning channels, the user dictionary, typo corrections, auto words.
  ASSERT_EQ(exported.manifest.items.size(), 5u);
  EXPECT_EQ(exported.manifest.items[0].name, "learning");
  EXPECT_EQ(exported.manifest.items[0].file, "learning.tsv.enc");
  EXPECT_EQ(exported.manifest.items[1].name, "learning_english");

  Stores restored;
  const auto imported = host::ImportLearningData(restored.View(), kAllStores, archive,
                                                 learning::ImportConflictPolicy::Merge, kKeyA);
  ASSERT_EQ(imported.error, host::BackupError::None);
  EXPECT_EQ(imported.counts.at("learning").imported, 3u);
  EXPECT_EQ(imported.counts.at("user_dictionary").imported, 1u);
  EXPECT_EQ(restored.Texts(), source.Texts());
}

TEST(LearningDataManagerTest, PlaintextExportRoundTrips) {
  ScopedDirectory directory;
  const auto archive = directory.File("plain.zip");
  Stores source;
  source.Fill();
  const auto exported = host::ExportLearningData(source.View(), {LearningDataStore::Typo}, archive,
                                                 false, kKeyA, host::BackupManifest{});
  ASSERT_EQ(exported.error, host::BackupError::None);
  ASSERT_EQ(exported.manifest.items.size(), 1u);
  EXPECT_EQ(exported.manifest.items[0].file, "typo_corrections.tsv");
  EXPECT_FALSE(exported.manifest.encrypted);

  Stores restored;
  const auto imported = host::ImportLearningData(restored.View(), kAllStores, archive,
                                                 learning::ImportConflictPolicy::Merge, kKeyB);
  ASSERT_EQ(imported.error, host::BackupError::None);
  EXPECT_EQ(restored.typo.SerializeText(), source.typo.SerializeText());
  EXPECT_EQ(restored.kana.size(), 0u);
}

TEST(LearningDataManagerTest, ImportWithAnotherKeyFailsWithoutTouchingStores) {
  ScopedDirectory directory;
  const auto archive = directory.File("other-user.zip");
  Stores source;
  source.Fill();
  ASSERT_EQ(host::ExportLearningData(source.View(), kAllStores, archive, true, kKeyA,
                                     host::BackupManifest{})
                .error,
            host::BackupError::None);

  Stores target;
  target.kana.Observe("ほか", "他", 0.8, kNow);
  const auto before = target.Texts();
  const auto imported = host::ImportLearningData(target.View(), kAllStores, archive,
                                                 learning::ImportConflictPolicy::Overwrite, kKeyB);
  EXPECT_EQ(imported.error, host::BackupError::DecryptFailed);
  EXPECT_TRUE(imported.counts.empty());
  EXPECT_EQ(target.Texts(), before);
}

TEST(LearningDataManagerTest, MalformedArchiveFailsWithoutTouchingStores) {
  ScopedDirectory directory;
  const auto archive = directory.File("broken.zip");
  {
    std::ofstream out(archive, std::ios::binary);
    out << "PK\x03\x04 definitely not a zip archive";
  }
  Stores target;
  target.Fill();
  const auto before = target.Texts();
  const auto imported = host::ImportLearningData(target.View(), kAllStores, archive,
                                                 learning::ImportConflictPolicy::Merge, kKeyA);
  EXPECT_NE(imported.error, host::BackupError::None);
  EXPECT_EQ(target.Texts(), before);

  const auto missing =
      host::ImportLearningData(target.View(), kAllStores, directory.File("missing.zip"),
                               learning::ImportConflictPolicy::Merge, kKeyA);
  EXPECT_EQ(missing.error, host::BackupError::SourceMissing);
}

TEST(LearningDataManagerTest, ConflictPoliciesApplyToEachStore) {
  ScopedDirectory directory;
  const auto archive = directory.File("conflicts.zip");
  Stores source;
  source.kana.Observe("にほん", "日本", 0.5, kNow);
  source.typo.Observe("こんちには", "こんにちは", kNow);
  source.user_dictionary.Add({"東京特許", "とうきょうとっきょ", std::nullopt, std::nullopt, 9.0});
  ASSERT_EQ(host::ExportLearningData(source.View(), kAllStores, archive, true, kKeyA,
                                     host::BackupManifest{})
                .error,
            host::BackupError::None);

  const auto run = [&](learning::ImportConflictPolicy policy, Stores& target) {
    target.kana.Observe("にほん", "日本", 1.0, kNow);
    target.typo.Observe("こんちには", "こんにちは", kNow);
    target.user_dictionary.Add({"東京特許", "とうきょうとっきょ", std::nullopt, std::nullopt, 1.0});
    const auto result = host::ImportLearningData(target.View(), kAllStores, archive, policy, kKeyA);
    EXPECT_EQ(result.error, host::BackupError::None);
    return result;
  };

  Stores merged;
  const auto merge_result = run(learning::ImportConflictPolicy::Merge, merged);
  EXPECT_EQ(merge_result.counts.at("learning").conflicts, 1u);
  EXPECT_DOUBLE_EQ(merged.kana.Score("にほん", "日本", kNow), 1.5);
  EXPECT_EQ(merged.typo.All().front().record.count, 2u);
  EXPECT_EQ(merged.user_dictionary.All().front().value, 1.0);

  Stores overwritten;
  run(learning::ImportConflictPolicy::Overwrite, overwritten);
  EXPECT_DOUBLE_EQ(overwritten.kana.Score("にほん", "日本", kNow), 0.5);
  EXPECT_EQ(overwritten.typo.All().front().record.count, 1u);
  EXPECT_EQ(overwritten.user_dictionary.All().front().value, 9.0);

  Stores kept;
  const auto keep_result = run(learning::ImportConflictPolicy::KeepBoth, kept);
  EXPECT_EQ(keep_result.counts.at("learning").skipped, 1u);
  EXPECT_DOUBLE_EQ(kept.kana.Score("にほん", "日本", kNow), 1.0);
  EXPECT_EQ(kept.user_dictionary.All().front().value, 1.0);
}

TEST(LearningDataManagerTest, ItemWithMalformedRowsFailsWithoutTouchingStores) {
  ScopedDirectory directory;
  const auto archive = directory.File("tampered.zip");
  host::BackupManifest written;
  uint64_t size = 0;
  auto error = host::BackupError::None;
  // A well-formed archive whose learning item is not what a Host writes.
  ASSERT_TRUE(host::WriteBackupArchive(archive,
                                       {{"learning", "learning.tsv.enc", 2,
                                         "# azookey-learning-tsv escaped=1 version=2\nnot a row\n"},
                                        {"typo_corrections", "typo_corrections.tsv.enc", 0, ""}},
                                       true, kKeyA, host::BackupManifest{}, &written, &size,
                                       &error));

  Stores target;
  target.Fill();
  const auto before = target.Texts();
  const auto imported = host::ImportLearningData(target.View(), kAllStores, archive,
                                                 learning::ImportConflictPolicy::Merge, kKeyA);
  EXPECT_EQ(imported.error, host::BackupError::BadItem);
  EXPECT_TRUE(imported.counts.empty());
  EXPECT_EQ(target.Texts(), before);
}

TEST(LearningDataManagerTest, StoreThatRefusesToSaveFailsExportAndImport) {
  ScopedDirectory directory;
  const auto learning_path = directory.File("learning.tsv");
  {
    std::ofstream out(azookey::learning::EncryptedPathFor(
                          azookey::learning::LearningStoreV2PathFor(learning_path)),
                      std::ios::binary);
    out << "undecipherable";
  }
  Stores stores;
  learning::LearningStore blocked(learning_path, &kKeyA);
  ASSERT_FALSE(blocked.Load());
  ASSERT_TRUE(blocked.save_blocked());
  auto view = stores.View();
  view.learning[0].store = &blocked;

  const auto exported = host::ExportLearningData(view, kAllStores, directory.File("out.zip"), true,
                                                 kKeyA, host::BackupManifest{});
  EXPECT_EQ(exported.error, host::BackupError::StoreUnavailable);
  EXPECT_FALSE(std::filesystem::exists(directory.File("out.zip")));
  const auto imported = host::ImportLearningData(view, kAllStores, directory.File("in.zip"),
                                                 learning::ImportConflictPolicy::Merge, kKeyA);
  EXPECT_EQ(imported.error, host::BackupError::StoreUnavailable);
  // Other selections still work.
  stores.Fill();
  EXPECT_EQ(host::ExportLearningData(view, {LearningDataStore::Typo}, directory.File("typo.zip"),
                                     true, kKeyA, host::BackupManifest{})
                .error,
            host::BackupError::None);
}

TEST(LearningDataManagerTest, ManifestCountLeavesOutForgottenPairs) {
  ScopedDirectory directory;
  Stores stores;
  stores.Fill();
  ASSERT_TRUE(host::ForgetLearningPair(stores.View(), "にほん", "日本"));
  const auto exported =
      host::ExportLearningData(stores.View(), {LearningDataStore::Learning},
                               directory.File("count.zip"), true, kKeyA, host::BackupManifest{});
  ASSERT_EQ(exported.error, host::BackupError::None);
  // "にほんご" has a global and a code.exe row; "にほん" was forgotten.
  EXPECT_EQ(exported.manifest.items[0].count, 2u);
}

TEST(LearningDataManagerTest, PathNamingAnAlternateDataStreamIsRejected) {
  ScopedDirectory directory;
  Stores stores;
  stores.Fill();
  const auto exported = host::ExportLearningData(
      stores.View(), kAllStores, directory.File("a:b.zip"), true, kKeyA, host::BackupManifest{});
  EXPECT_EQ(exported.error, host::BackupError::InvalidPath);
}

TEST(LearningDataManagerTest, DevicePathsReservedNamesAndLinksAreRejected) {
  ScopedDirectory directory;
  Stores stores;
  stores.Fill();
  const auto export_to = [&](const std::filesystem::path& path) {
    return host::ExportLearningData(stores.View(), kAllStores, path, true, kKeyA,
                                    host::BackupManifest{})
        .error;
  };
  EXPECT_EQ(export_to(directory.File("NUL.zip")), host::BackupError::InvalidPath);
  EXPECT_EQ(export_to(directory.File("com1.backup.zip")), host::BackupError::InvalidPath);
#ifdef _WIN32
  EXPECT_EQ(export_to(L"\\\\?\\C:\\azookey-backup.zip"), host::BackupError::InvalidPath);
  EXPECT_EQ(export_to(L"\\\\.\\C:\\azookey-backup.zip"), host::BackupError::InvalidPath);
  EXPECT_EQ(export_to(L"\\\\server\\share\\azookey-backup.zip"), host::BackupError::InvalidPath);
#endif

  const auto archive = directory.File("real.zip");
  ASSERT_EQ(export_to(archive), host::BackupError::None);
  const auto link = directory.File("link.zip");
  std::error_code ec;
  std::filesystem::create_symlink(archive, link, ec);
  if (ec) GTEST_SKIP() << "symbolic links are not available: " << ec.message();
  const auto imported = host::ImportLearningData(stores.View(), kAllStores, link,
                                                 learning::ImportConflictPolicy::Merge, kKeyA);
  EXPECT_EQ(imported.error, host::BackupError::InvalidPath);
}
