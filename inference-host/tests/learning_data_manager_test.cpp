#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "azookey/host/EnglishCandidates.h"
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

// In-memory stores; nothing here is saved to disk except the typo store,
// whose forget writes its file itself, in a directory of its own.
struct Stores {
  Stores() { EXPECT_EQ(typo.Load(kNow).mode, learning::TypoLearningMode::V2); }

  learning::LearningStore kana{"kana.tsv", &kKeyA};
  learning::LearningStore english{"english.tsv", &kKeyA};
  learning::UserDictionary user_dictionary{"user_dict.json", &kKeyA};
  ScopedDirectory typo_directory;
  learning::TypoLearningStore typo{typo_directory.File("typo.tsv"), &kKeyA};
  learning::AutoWordStore auto_word{"auto_words.tsv", &kKeyA};

  host::LearningDataStores View() {
    return host::LearningDataStores{{{std::string(host::kKanaLearningChannel), &kana},
                                     {std::string(host::kEnglishLearningChannel), &english}},
                                    &user_dictionary,
                                    &typo,
                                    &auto_word,
                                    kNow};
  }

  void Fill() {
    kana.Observe("にほん", "日本", 0.8, kNow);
    kana.Observe("にほんご", "日本語", 1.6, kNow);
    kana.ObserveEvent({"にほんご", "日本語", "code.exe", learning::LearningEventType::Commit, ""},
                      0.8, kNow);
    english.Observe("helo", "hello", 0.8, kNow);
    user_dictionary.Add({"東京特許", "とうきょうとっきょ", std::nullopt, std::nullopt, 1.5});
    typo.ObserveReadingPair("こんちには", "こんにちは", kNow);
    auto_word.Observe("推し活", "おしかつ", kNow, 3, false);
  }

  std::vector<std::string> Texts() const {
    return {kana.SerializeText(),
            english.SerializeText(),
            user_dictionary.SerializeText(),
            typo.SerializeReadingPairsText(),
            typo.SerializePatternsText(),
            auto_word.SerializeText()};
  }
};

const std::vector<LearningDataStore> kAllStores = {
    LearningDataStore::Learning, LearningDataStore::UserDictionary, LearningDataStore::Typo,
    LearningDataStore::AutoWord};

// A pattern event in the abstraction tokens the typo store accepts
// (typo-correction-learning-spec section 12.8).
learning::TypoPatternEvent PatternEvent(std::string observed, std::string intended,
                                        std::string typo_type, learning::TypoPatternOutcome outcome,
                                        uint64_t now) {
  learning::TypoPatternEvent event;
  event.observed_pattern = std::move(observed);
  event.intended_pattern = std::move(intended);
  event.typo_type = std::move(typo_type);
  event.observed_reading = "こうじしょう";
  event.corrected_reading = "こうしょう";
  event.outcome = outcome;
  event.now_epoch_sec = now;
  return event;
}

}  // namespace

TEST(LearningDataManagerTest, StoreNamesRoundTrip) {
  for (const auto store : kAllStores) {
    EXPECT_EQ(host::ParseLearningDataStore(host::LearningDataStoreName(store)), store);
  }
  EXPECT_FALSE(host::ParseLearningDataStore("dictionary").has_value());
}

TEST(LearningDataManagerTest, UserDictionaryPresenceLoadsLegacyAndEncryptedFiles) {
  ScopedDirectory temp;
  const auto path = temp.File("user_dict.json");
  learning::UserDictionary dictionary(path, &kKeyA);
  EXPECT_FALSE(host::detail::UserDictionaryFileExists(dictionary));
  {
    std::ofstream out(path);
    out << R"({"entries":[{"word":"legacy","ruby":"legacy"}]})";
    ASSERT_TRUE(out.good());
  }
  ASSERT_TRUE(host::detail::UserDictionaryFileExists(dictionary));
  ASSERT_TRUE(dictionary.Load());
  EXPECT_EQ(dictionary.Lookup("legacy").size(), 1u);
  EXPECT_FALSE(std::filesystem::exists(path));
  ASSERT_TRUE(std::filesystem::exists(dictionary.storage_path()));

  learning::UserDictionary reader(path, &kKeyA);
  ASSERT_TRUE(host::detail::UserDictionaryFileExists(reader));
  ASSERT_TRUE(reader.Load());
  EXPECT_EQ(reader.Lookup("legacy").size(), 1u);
}

TEST(LearningDataManagerTest, UserDictionaryPresenceDoesNotHideUnreadableCiphertext) {
  ScopedDirectory temp;
  const auto path = temp.File("user_dict.json");
  {
    std::ofstream out(learning::EncryptedPathFor(path));
    out << "invalid ciphertext";
    ASSERT_TRUE(out.good());
  }
  {
    std::ofstream out(path);
    out << R"({"entries":[{"word":"legacy","ruby":"legacy"}]})";
    ASSERT_TRUE(out.good());
  }
  learning::UserDictionary dictionary(path, &kKeyA);
  ASSERT_TRUE(host::detail::UserDictionaryFileExists(dictionary));
  EXPECT_FALSE(dictionary.Load());
  EXPECT_TRUE(dictionary.save_blocked());
  EXPECT_FALSE(dictionary.Save());
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_TRUE(std::filesystem::exists(dictionary.storage_path()));
  learning::UserDictionary default_crypto(path);
  EXPECT_TRUE(host::detail::UserDictionaryFileExists(default_crypto));
}

TEST(LearningDataManagerTest, UserDictionaryPresenceRejectsUnusablePath) {
  ScopedDirectory temp;
  // Platforms may report this as missing or as a filesystem error.
  const std::string invalid_name(512, 'x');
  const auto path = temp.File(invalid_name.c_str());
  std::error_code ec;
  EXPECT_FALSE(std::filesystem::exists(path, ec));
  learning::UserDictionary dictionary(path, &kKeyA);
  EXPECT_FALSE(host::detail::UserDictionaryFileExists(dictionary));
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
  EXPECT_TRUE(stores.typo.ReadingPairs().empty());
  EXPECT_TRUE(host::ForgetLearningEntry(view, LearningDataStore::AutoWord,
                                        first(LearningDataStore::AutoWord)));
  // A rejected word stays recorded so that it is not proposed again.
  EXPECT_EQ(stores.auto_word.ListByState(learning::AutoWordState::Rejected).size(), 1u);

  EXPECT_FALSE(host::ForgetLearningEntry(view, LearningDataStore::Learning, "0000000000000000"));
}

// typo-correction-learning-spec section 12.8: the patterns are listed in
// channel "pattern", one entry per (observed, intended) over its typo types,
// and forgetting the entry removes all of them and leaves the reading pairs.
TEST(LearningDataManagerTest, TypoPatternsAreListedPerPairAndForgottenTogether) {
  using learning::TypoPatternOutcome;
  Stores stores;
  stores.Fill();
  ASSERT_TRUE(stores.typo
                  .RecordPatternEvent(PatternEvent("j_insert_after_u", "j_delete_after_u",
                                                   "extra_char", TypoPatternOutcome::Accept, kNow))
                  .has_value());
  ASSERT_TRUE(
      stores.typo
          .RecordPatternEvent(PatternEvent("j_insert_after_u", "j_delete_after_u", "adjacent_key",
                                           TypoPatternOutcome::Reject, kNow + 5))
          .has_value());
  ASSERT_TRUE(stores.typo
                  .RecordPatternEvent(PatternEvent("z_sub_x", "x_sub_z", "adjacent_key",
                                                   TypoPatternOutcome::Accept, kNow))
                  .has_value());
  ASSERT_EQ(stores.typo.Patterns().size(), 3u);

  const auto view = stores.View();
  const auto listed = host::ListLearningEntries(view, LearningDataStore::Typo, "", 0, 10);
  // The reading pair and two pattern entries.
  ASSERT_EQ(listed.total, 3u);
  const auto folded =
      std::find_if(listed.entries.begin(), listed.entries.end(),
                   [](const auto& entry) { return entry.reading == "j_insert_after_u"; });
  ASSERT_NE(folded, listed.entries.end());
  EXPECT_EQ(folded->channel, host::kTypoPatternChannel);
  EXPECT_EQ(folded->surface, "j_delete_after_u");
  EXPECT_EQ(folded->id, host::LearningEntryId(LearningDataStore::Typo, host::kTypoPatternChannel,
                                              "j_insert_after_u", "j_delete_after_u"));
  EXPECT_DOUBLE_EQ(folded->weight, 1.0);
  EXPECT_EQ(folded->metadata.at("accept_count"), "1");
  EXPECT_EQ(folded->metadata.at("reject_count"), "1");
  EXPECT_EQ(folded->last_updated_epoch_sec, kNow + 5);

  EXPECT_TRUE(host::ForgetLearningEntry(view, LearningDataStore::Typo, folded->id));
  const auto patterns = stores.typo.Patterns();
  ASSERT_EQ(patterns.size(), 1u);
  EXPECT_EQ(patterns[0].observed_pattern, "z_sub_x");
  EXPECT_EQ(stores.typo.ReadingPairs().size(), 1u);
  EXPECT_FALSE(host::ForgetLearningEntry(view, LearningDataStore::Typo, folded->id));
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
  // Two learning channels, the user dictionary, typo corrections and
  // patterns, auto words.
  ASSERT_EQ(exported.manifest.items.size(), 6u);
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

// M63-B (mixed-english-auto-conversion-spec section 11): the negative
// observation row of the English channel is an ordinary learning row: listed,
// forgotten (which lifts it) and carried by export and import as it is.
TEST(LearningDataManagerTest, EnglishNegativeObservationIsAnOrdinaryRow) {
  Stores source;
  source.Fill();
  learning::LearningRecord negative;
  negative.reject_count = 1;
  negative.last_event = learning::LearningEventType::CorrectionReject;
  negative.last_updated_epoch_sec = kNow;
  const auto reading = host::EnglishNegativeObservationReading("github");
  source.english.Set(reading, "github", negative);
  const auto view = source.View();
  const auto listed = host::ListLearningEntries(view, LearningDataStore::Learning, "github", 0, 10);
  ASSERT_EQ(listed.total, 1u);
  EXPECT_EQ(listed.entries[0].channel, "english");
  EXPECT_EQ(listed.entries[0].reading,
            "\xC2\xAC"
            "github");
  EXPECT_EQ(listed.entries[0].surface, "github");
  EXPECT_DOUBLE_EQ(listed.entries[0].weight, 0.0);
  EXPECT_EQ(listed.entries[0].metadata.at("reject_count"), "1");

  ScopedDirectory directory;
  const auto archive = directory.File("negative.zip");
  ASSERT_EQ(host::ExportLearningData(view, {LearningDataStore::Learning}, archive, true, kKeyA,
                                     host::BackupManifest{})
                .error,
            host::BackupError::None);
  Stores restored;
  ASSERT_EQ(host::ImportLearningData(restored.View(), {LearningDataStore::Learning}, archive,
                                     learning::ImportConflictPolicy::Merge, kKeyA)
                .error,
            host::BackupError::None);
  EXPECT_EQ(restored.english.SerializeText(), source.english.SerializeText());

  EXPECT_TRUE(host::ForgetLearningEntry(view, LearningDataStore::Learning, listed.entries[0].id));
  EXPECT_EQ(host::ListLearningEntries(view, LearningDataStore::Learning, "github", 0, 10).total,
            0u);
}

TEST(LearningDataManagerTest, PlaintextExportRoundTrips) {
  ScopedDirectory directory;
  const auto archive = directory.File("plain.zip");
  Stores source;
  source.Fill();
  const auto exported = host::ExportLearningData(source.View(), {LearningDataStore::Typo}, archive,
                                                 false, kKeyA, host::BackupManifest{});
  ASSERT_EQ(exported.error, host::BackupError::None);
  ASSERT_EQ(exported.manifest.items.size(), 2u);
  EXPECT_EQ(exported.manifest.items[0].file, "typo_corrections.tsv");
  EXPECT_EQ(exported.manifest.items[1].file, "typo_patterns.tsv");
  EXPECT_FALSE(exported.manifest.encrypted);

  Stores restored;
  const auto imported = host::ImportLearningData(restored.View(), kAllStores, archive,
                                                 learning::ImportConflictPolicy::Merge, kKeyB);
  ASSERT_EQ(imported.error, host::BackupError::None);
  EXPECT_EQ(restored.typo.SerializeReadingPairsText(), source.typo.SerializeReadingPairsText());
  EXPECT_EQ(restored.kana.size(), 0u);
}

// learning-data-management-spec section 5: the v2 typo store adds item
// typo_patterns, which an import merges back.
TEST(LearningDataManagerTest, TypoPatternsRoundTripThroughTheBackup) {
  using learning::TypoPatternOutcome;
  ScopedDirectory directory;
  const auto archive = directory.File("patterns.zip");
  Stores source;
  source.Fill();
  ASSERT_TRUE(source.typo
                  .RecordPatternEvent(PatternEvent("j_insert_after_u", "j_delete_after_u",
                                                   "extra_char", TypoPatternOutcome::Accept, kNow))
                  .has_value());
  ASSERT_TRUE(source.typo
                  .RecordPatternEvent(PatternEvent("z_sub_x", "x_sub_z", "adjacent_key",
                                                   TypoPatternOutcome::Reject, kNow + 1))
                  .has_value());
  const auto exported = host::ExportLearningData(source.View(), {LearningDataStore::Typo}, archive,
                                                 true, kKeyA, host::BackupManifest{});
  ASSERT_EQ(exported.error, host::BackupError::None);
  ASSERT_EQ(exported.manifest.items.size(), 2u);
  EXPECT_EQ(exported.manifest.items[1].name, "typo_patterns");
  EXPECT_EQ(exported.manifest.items[1].file, "typo_patterns.tsv.enc");
  EXPECT_EQ(exported.manifest.items[1].count, 2u);

  Stores restored;
  const auto imported =
      host::ImportLearningData(restored.View(), {LearningDataStore::Typo}, archive,
                               learning::ImportConflictPolicy::Merge, kKeyA);
  ASSERT_EQ(imported.error, host::BackupError::None);
  EXPECT_EQ(imported.counts.at("typo_patterns").imported, 2u);
  EXPECT_EQ(imported.counts.at("typo_corrections").imported, 1u);
  EXPECT_EQ(restored.typo.SerializePatternsText(), source.typo.SerializePatternsText());
  EXPECT_EQ(restored.typo.SerializeReadingPairsText(), source.typo.SerializeReadingPairsText());
}

namespace {

// kKeyA that can refuse to encrypt, so a typo store's v1 migration fails.
class RefusableCrypto final : public learning::ByteCrypto {
 public:
  bool Encrypt(const std::vector<uint8_t>& plain, std::vector<uint8_t>& cipher) const override {
    return !refuse_encrypt && kKeyA.Encrypt(plain, cipher);
  }
  bool Decrypt(const std::vector<uint8_t>& cipher, std::vector<uint8_t>& plain) const override {
    return kKeyA.Decrypt(cipher, plain);
  }
  bool refuse_encrypt{false};
};

}  // namespace

// A typo store that reads v1 as it is (typo-correction-learning-spec section
// 12.8) has no patterns: its export writes typo_corrections alone, and an
// import counts the typo_patterns item as skipped (spec section 5).
TEST(LearningDataManagerTest, TypoStoreReadingV1AsItIsSkipsPatterns) {
  ScopedDirectory directory;
  const auto v1_path = directory.File("typo.tsv");
  {
    learning::TypoCorrectionStore v1(v1_path, &kKeyA);
    ASSERT_TRUE(v1.Observe("こんちには", "こんにちは", kNow));
    ASSERT_TRUE(v1.Save());
  }
  RefusableCrypto crypto;
  crypto.refuse_encrypt = true;
  learning::TypoLearningStore typo(v1_path, &crypto);
  ASSERT_EQ(typo.Load(kNow).mode, learning::TypoLearningMode::V1Fallback);
  crypto.refuse_encrypt = false;
  const host::LearningDataStores view{{}, nullptr, &typo, nullptr, kNow};

  const auto exported_archive = directory.File("v1.zip");
  const auto exported = host::ExportLearningData(view, {LearningDataStore::Typo}, exported_archive,
                                                 true, kKeyA, host::BackupManifest{});
  ASSERT_EQ(exported.error, host::BackupError::None);
  ASSERT_EQ(exported.manifest.items.size(), 1u);
  EXPECT_EQ(exported.manifest.items[0].name, "typo_corrections");
  EXPECT_EQ(exported.manifest.items[0].count, 1u);

  const auto patterns_archive = directory.File("patterns.zip");
  Stores source;
  ASSERT_TRUE(
      source.typo
          .RecordPatternEvent(PatternEvent("j_insert_after_u", "j_delete_after_u", "extra_char",
                                           learning::TypoPatternOutcome::Accept, kNow))
          .has_value());
  ASSERT_EQ(host::ExportLearningData(source.View(), {LearningDataStore::Typo}, patterns_archive,
                                     true, kKeyA, host::BackupManifest{})
                .error,
            host::BackupError::None);
  const auto imported = host::ImportLearningData(view, {LearningDataStore::Typo}, patterns_archive,
                                                 learning::ImportConflictPolicy::Merge, kKeyA);
  ASSERT_EQ(imported.error, host::BackupError::None);
  EXPECT_EQ(imported.counts.at("typo_patterns").imported, 0u);
  EXPECT_EQ(imported.counts.at("typo_patterns").skipped, 1u);
  EXPECT_TRUE(typo.Patterns().empty());
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
  source.typo.ObserveReadingPair("こんちには", "こんにちは", kNow);
  source.user_dictionary.Add({"東京特許", "とうきょうとっきょ", std::nullopt, std::nullopt, 9.0});
  ASSERT_EQ(host::ExportLearningData(source.View(), kAllStores, archive, true, kKeyA,
                                     host::BackupManifest{})
                .error,
            host::BackupError::None);

  const auto run = [&](learning::ImportConflictPolicy policy, Stores& target) {
    target.kana.Observe("にほん", "日本", 1.0, kNow);
    target.typo.ObserveReadingPair("こんちには", "こんにちは", kNow);
    target.user_dictionary.Add({"東京特許", "とうきょうとっきょ", std::nullopt, std::nullopt, 1.0});
    const auto result = host::ImportLearningData(target.View(), kAllStores, archive, policy, kKeyA);
    EXPECT_EQ(result.error, host::BackupError::None);
    return result;
  };

  Stores merged;
  const auto merge_result = run(learning::ImportConflictPolicy::Merge, merged);
  EXPECT_EQ(merge_result.counts.at("learning").conflicts, 1u);
  EXPECT_DOUBLE_EQ(merged.kana.Score("にほん", "日本", kNow), 1.5);
  EXPECT_EQ(merged.typo.ReadingPairs().front().record.count, 2u);
  EXPECT_EQ(merged.user_dictionary.All().front().value, 1.0);

  Stores overwritten;
  run(learning::ImportConflictPolicy::Overwrite, overwritten);
  EXPECT_DOUBLE_EQ(overwritten.kana.Score("にほん", "日本", kNow), 0.5);
  EXPECT_EQ(overwritten.typo.ReadingPairs().front().record.count, 1u);
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

// A typo_patterns row the store cannot read fails the import as any other
// item does, before any store changes.
TEST(LearningDataManagerTest, MalformedTypoPatternsItemFailsWithoutTouchingStores) {
  ScopedDirectory directory;
  const auto archive = directory.File("tampered-patterns.zip");
  host::BackupManifest written;
  uint64_t size = 0;
  auto error = host::BackupError::None;
  ASSERT_TRUE(host::WriteBackupArchive(archive,
                                       {{"typo_patterns", "typo_patterns.tsv.enc", 1,
                                         "# azookey-typo-patterns-tsv escaped=1\nnot a row\n"}},
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

// The user dictionary text import shares this check with the backup import.
TEST(LearningDataManagerTest, UserDataSourcePathRefusesNetworkAndDeviceNamespaces) {
  ScopedDirectory directory;
  EXPECT_TRUE(host::IsAcceptableUserDataSourcePath(directory.File("dictionary.txt")));
#ifdef _WIN32
  EXPECT_TRUE(host::IsAcceptableUserDataSourcePath(L"C:\\dictionary.txt"));
  EXPECT_TRUE(host::IsAcceptableUserDataSourcePath(L"c:/dictionary.txt"));
  EXPECT_FALSE(host::IsAcceptableUserDataSourcePath(L"/??/UNC/server/share/d.txt"));
  EXPECT_FALSE(host::IsAcceptableUserDataSourcePath(L"C:d.txt"));
  for (const wchar_t* path :
       {L"\\??\\UNC\\server\\share\\d.txt", L"\\??\\C:\\d.txt", L"\\\\?\\C:\\d.txt",
        L"\\\\.\\C:\\d.txt", L"//?/C:/d.txt", L"//./C:/d.txt", L"\\\\server\\share\\d.txt",
        L"//server/share/d.txt", L"\\\\?\\UNC\\server\\share\\d.txt",
        L"\\??\\unc\\server\\share\\d.txt"}) {
    EXPECT_FALSE(host::IsAcceptableUserDataSourcePath(path));
  }
#endif
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
  // The NT namespace and mixed separators: only a plain "X:\" root is accepted.
  EXPECT_EQ(export_to(L"\\??\\UNC\\server\\share\\azookey-backup.zip"),
            host::BackupError::InvalidPath);
  EXPECT_EQ(export_to(L"\\??\\C:\\azookey-backup.zip"), host::BackupError::InvalidPath);
  EXPECT_EQ(export_to(L"//?/C:/azookey-backup.zip"), host::BackupError::InvalidPath);
  EXPECT_EQ(export_to(L"//./C:/azookey-backup.zip"), host::BackupError::InvalidPath);
  EXPECT_EQ(export_to(L"\\\\?\\UNC\\server\\share\\azookey-backup.zip"),
            host::BackupError::InvalidPath);
  EXPECT_EQ(export_to(L"/\\server\\share\\azookey-backup.zip"), host::BackupError::InvalidPath);
  EXPECT_EQ(export_to(L"/??/UNC/server/share/azookey-backup.zip"), host::BackupError::InvalidPath);
  EXPECT_EQ(export_to(L"C:azookey-backup.zip"), host::BackupError::InvalidPath);
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
