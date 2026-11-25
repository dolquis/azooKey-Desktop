#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "TestByteCrypto.h"
#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/TypoCorrectionStore.h"
#include "azookey/learning/TypoLearningStore.h"
#include "azookey/learning/TypoPattern.h"

namespace {

using azookey::learning::ByteCrypto;
using azookey::learning::ComputeTypoConfidence;
using azookey::learning::EncryptedPathFor;
using azookey::learning::ImportConflictPolicy;
using azookey::learning::TypoCorrectionEntry;
using azookey::learning::TypoCorrectionRecord;
using azookey::learning::TypoCorrectionStore;
using azookey::learning::TypoForgetResult;
using azookey::learning::TypoLearningLimits;
using azookey::learning::TypoLearningLoadIssue;
using azookey::learning::TypoLearningMode;
using azookey::learning::TypoLearningStore;
using azookey::learning::TypoPatternEvent;
using azookey::learning::TypoPatternOutcome;
using azookey::learning::TypoPatternRecord;
using azookey::learning::TypoStoreOrigin;

constexpr uint64_t kNow = 1'700'000'000;
constexpr uint64_t kDay = 24 * 60 * 60;
constexpr std::string_view kHeader = "# azookey-typo-correction-tsv escaped=1\n";

// The test cipher, with a switch that makes every write fail while reads keep
// working (a failed save after a successful load).
class SwitchableCrypto final : public ByteCrypto {
 public:
  bool Encrypt(const std::vector<uint8_t>& plain, std::vector<uint8_t>& cipher) const override {
    return !fail_encrypt && inner_.Encrypt(plain, cipher);
  }
  bool Decrypt(const std::vector<uint8_t>& cipher, std::vector<uint8_t>& plain) const override {
    if (!inner_.Decrypt(cipher, plain)) return false;
    // Refuses only a database (the v2 store), so v1 files still read.
    static constexpr char kDatabaseHeader[] = "SQLite format 3";
    return !(fail_database_decrypt && plain.size() >= sizeof(kDatabaseHeader) &&
             std::memcmp(plain.data(), kDatabaseHeader, sizeof(kDatabaseHeader)) == 0);
  }
  bool fail_encrypt{false};
  bool fail_database_decrypt{false};

 private:
  azookey::learning::test::TestByteCrypto inner_;
};

// SQLite takes its allocator once per process, before any other call; a store
// load does that before the tests use the SQLite API directly.
class SqliteEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("azookey_typo_v2_env_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    SwitchableCrypto crypto;
    TypoLearningStore probe(dir / "typo_corrections.tsv", &crypto);
    const auto result = probe.Load(kNow);
    ASSERT_EQ(result.issue, TypoLearningLoadIssue::None);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

[[maybe_unused]] const auto* const kSqliteEnvironment =
    ::testing::AddGlobalTestEnvironment(new SqliteEnvironment);

std::string ReadBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
}

bool Exists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

bool Contains(const std::string& haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

// Runs `sql` on a serialized database and returns the result serialized.
std::string ExecOnDatabase(const std::string& bytes, const char* sql) {
  sqlite3* db = nullptr;
  EXPECT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
  auto* buffer = static_cast<unsigned char*>(sqlite3_malloc64(bytes.size()));
  std::memcpy(buffer, bytes.data(), bytes.size());
  const auto size = static_cast<sqlite3_int64>(bytes.size());
  EXPECT_EQ(sqlite3_deserialize(db, "main", buffer, size, size,
                                SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE),
            SQLITE_OK);
  EXPECT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), SQLITE_OK);
  sqlite3_int64 out_size = 0;
  unsigned char* out = sqlite3_serialize(db, "main", &out_size, 0);
  std::string result(reinterpret_cast<const char*>(out), static_cast<size_t>(out_size));
  sqlite3_free(out);
  sqlite3_close(db);
  return result;
}

// Whether `sql` succeeds on a serialized database (the bytes are not changed).
bool ExecSucceeds(const std::string& bytes, const char* sql) {
  sqlite3* db = nullptr;
  if (sqlite3_open(":memory:", &db) != SQLITE_OK) return false;
  auto* buffer = static_cast<unsigned char*>(sqlite3_malloc64(bytes.size()));
  std::memcpy(buffer, bytes.data(), bytes.size());
  const auto size = static_cast<sqlite3_int64>(bytes.size());
  const bool opened = sqlite3_deserialize(db, "main", buffer, size, size,
                                          SQLITE_DESERIALIZE_FREEONCLOSE |
                                              SQLITE_DESERIALIZE_RESIZEABLE) == SQLITE_OK;
  const bool ok = opened && sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
  sqlite3_close(db);
  return ok;
}

size_t CountCorruptCopies(const std::filesystem::path& dir) {
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().filename().string().find(".corrupt-") != std::string::npos) ++count;
  }
  return count;
}

// The SQLite header keeps user_version big-endian at offset 60.
std::string WithUserVersion(std::string bytes, uint32_t version) {
  for (int i = 0; i < 4; ++i) {
    bytes[60 + i] = static_cast<char>((version >> (8 * (3 - i))) & 0xff);
  }
  return bytes;
}

TypoPatternEvent PatternEvent(std::string observed, std::string intended,
                              TypoPatternOutcome outcome, uint64_t now, std::string app_name = {}) {
  TypoPatternEvent event;
  event.observed_pattern = std::move(observed);
  event.intended_pattern = std::move(intended);
  event.typo_type = "extra_char";
  event.observed_reading = "こうじしょう";
  event.corrected_reading = "こうしょう";
  event.selected_surface = "交渉";
  event.app_name = std::move(app_name);
  event.left_context_hash = "0a1b2c3d";
  event.outcome = outcome;
  event.now_epoch_sec = now;
  return event;
}

class TypoLearningStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // A name of its own per test and run, so parallel worktrees and test
    // processes never share or delete each other's files. It stays short: the
    // store appends ".corrupt-<epoch>" and a temp suffix, and a long TMP plus a
    // long test name would pass MAX_PATH (DEV-2190).
    static std::atomic<unsigned> sequence{0};
    const auto temp = std::filesystem::temp_directory_path();
    do {
      dir_ = temp / ("azt_" + std::to_string(std::random_device{}()) + "_" +
                     std::to_string(sequence.fetch_add(1)));
    } while (!std::filesystem::create_directory(dir_));
    v1_ = dir_ / "typo_corrections.tsv";
    paths_ = azookey::learning::TypoLearningPathsFor(v1_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  void WriteEncrypted(const std::filesystem::path& logical, const std::string& text) {
    std::vector<uint8_t> plain(text.begin(), text.end());
    std::vector<uint8_t> cipher;
    azookey::learning::test::TestByteCrypto().Encrypt(plain, cipher);
    WriteBytes(EncryptedPathFor(logical), std::string(cipher.begin(), cipher.end()));
  }

  void WriteUnreadable(const std::filesystem::path& logical) {
    WriteBytes(EncryptedPathFor(logical), "not a ciphertext");
  }

  std::optional<std::string> Decrypted(const std::filesystem::path& logical) {
    const auto bytes = ReadBytes(EncryptedPathFor(logical));
    std::vector<uint8_t> cipher(bytes.begin(), bytes.end());
    std::vector<uint8_t> plain;
    if (!Exists(EncryptedPathFor(logical)) || !crypto_.Decrypt(cipher, plain)) return std::nullopt;
    return std::string(plain.begin(), plain.end());
  }

  // The decrypted text; a missing or unreadable file fails the test instead of
  // dereferencing an empty optional.
  std::string DecryptedText(const std::filesystem::path& logical) {
    const auto text = Decrypted(logical);
    EXPECT_TRUE(text.has_value()) << "no readable file for " << logical.string();
    return text.value_or(std::string());
  }

  std::unique_ptr<TypoLearningStore> MakeStore(TypoLearningLimits limits = {}) {
    return std::make_unique<TypoLearningStore>(v1_, &crypto_, limits);
  }

  // A v2 store migrated from one v1 pair, saved with its mark.
  void MakeMigratedV2() {
    WriteEncrypted(v1_, std::string(kHeader) + "こんちには\tこんにちは\t3 1700000000\n");
    auto store = MakeStore();
    ASSERT_EQ(store->Load(kNow).mode, TypoLearningMode::V2);
  }

  std::filesystem::path dir_;
  std::filesystem::path v1_;
  azookey::learning::TypoLearningPaths paths_;
  SwitchableCrypto crypto_;
};

}  // namespace

TEST_F(TypoLearningStoreTest, PathsAreDerivedFromTheV1Name) {
  EXPECT_EQ(paths_.v2_store, dir_ / "typo_learning.v2.db");
  EXPECT_EQ(paths_.v1_backup, dir_ / "typo_corrections.v1.tsv.bak");
  const auto custom = azookey::learning::TypoLearningPathsFor(dir_ / "mine.tsv");
  EXPECT_EQ(custom.v2_store, dir_ / "typo_learning.v2.db");
  EXPECT_EQ(custom.v1_backup, dir_ / "mine.v1.tsv.bak");
  const auto bare = azookey::learning::TypoLearningPathsFor(dir_ / "typos");
  EXPECT_EQ(bare.v1_backup, dir_ / "typos.v1.bak");
}

TEST_F(TypoLearningStoreTest, WithNoFilesStartsEmptyAndMarksTheFirstSave) {
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::V2);
  EXPECT_FALSE(result.migrated);
  EXPECT_EQ(store->origin(), TypoStoreOrigin::Empty);
  ASSERT_TRUE(store->Save(kNow));

  auto reloaded = MakeStore();
  EXPECT_EQ(reloaded->Load(kNow).mode, TypoLearningMode::V2);
  EXPECT_EQ(reloaded->origin(), TypoStoreOrigin::Empty);
}

TEST_F(TypoLearningStoreTest, MigratesTheV1StoreAndMovesItAsideEncrypted) {
  // An old row is kept: no retention limit applies while migrating.
  const auto old_epoch = kNow - 400 * kDay;
  WriteEncrypted(v1_, std::string(kHeader) + "こんちには\tこんにちは\t3 1700000000\n" +
                          "きょうわ\tきょうは\t1 " + std::to_string(old_epoch) + "\n");
  const auto v1_bytes = ReadBytes(EncryptedPathFor(v1_));

  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::V2);
  EXPECT_TRUE(result.migrated);
  EXPECT_EQ(result.migrated_pairs, 2u);
  EXPECT_FALSE(result.v1_store_pending);
  EXPECT_EQ(store->origin(), TypoStoreOrigin::V1Migrated);
  EXPECT_FALSE(Exists(EncryptedPathFor(v1_)));
  EXPECT_EQ(ReadBytes(EncryptedPathFor(paths_.v1_backup)), v1_bytes);

  const auto pairs = store->ReadingPairs();
  ASSERT_EQ(pairs.size(), 2u);
  EXPECT_EQ(pairs[0].wrong_reading, "きょうわ");
  EXPECT_EQ(pairs[0].record.last_updated_epoch_sec, old_epoch);
  EXPECT_EQ(pairs[1].record.count, 3u);

  // The next start uses v2 as it is (decision 8).
  auto reloaded = MakeStore();
  const auto again = reloaded->Load(kNow);
  EXPECT_EQ(again.mode, TypoLearningMode::V2);
  EXPECT_FALSE(again.migrated);
  EXPECT_EQ(reloaded->ReadingPairs().size(), 2u);
}

TEST_F(TypoLearningStoreTest, PlaintextV1IsEncryptedByM34BeforeItMoves) {
  WriteBytes(v1_, std::string(kHeader) + "こんちには\tこんにちは\t3 1700000000\n");
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  ASSERT_EQ(result.mode, TypoLearningMode::V2);
  EXPECT_EQ(result.migrated_pairs, 1u);
  EXPECT_FALSE(Exists(v1_));
  EXPECT_TRUE(Exists(EncryptedPathFor(paths_.v1_backup)));
  // M34 keeps its plaintext backup; alone, it counts as no v1 store.
  auto m34_backup = v1_;
  m34_backup += ".bak";
  EXPECT_TRUE(Exists(m34_backup));

  auto reloaded = MakeStore();
  const auto again = reloaded->Load(kNow);
  EXPECT_EQ(again.mode, TypoLearningMode::V2);
  EXPECT_FALSE(again.v1_side_unreadable);
}

TEST_F(TypoLearningStoreTest, UnreadableV1StoreBlocksWithoutCreatingV2) {
  WriteUnreadable(v1_);
  WriteEncrypted(paths_.v1_backup, std::string(kHeader) + "こんちには\tこんにちは\t3 1\n");
  const auto v1_bytes = ReadBytes(EncryptedPathFor(v1_));
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::Blocked);
  EXPECT_EQ(result.issue, TypoLearningLoadIssue::V1StoreUnreadable);
  EXPECT_TRUE(store->ReadingPairs().empty());
  EXPECT_FALSE(store->Save(kNow));
  EXPECT_FALSE(Exists(EncryptedPathFor(paths_.v2_store)));
  EXPECT_EQ(ReadBytes(EncryptedPathFor(v1_)), v1_bytes);
}

TEST_F(TypoLearningStoreTest, MigratesFromTheBackupWhenTheV1StoreIsGone) {
  WriteEncrypted(paths_.v1_backup, std::string(kHeader) + "こんちには\tこんにちは\t4 1700000000\n");
  const auto backup_bytes = ReadBytes(EncryptedPathFor(paths_.v1_backup));
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::V2);
  EXPECT_EQ(result.migrated_pairs, 1u);
  EXPECT_EQ(store->origin(), TypoStoreOrigin::V1Migrated);
  EXPECT_EQ(ReadBytes(EncryptedPathFor(paths_.v1_backup)), backup_bytes);
  EXPECT_FALSE(Exists(EncryptedPathFor(v1_)));
}

TEST_F(TypoLearningStoreTest, UnreadableBackupBlocks) {
  WriteUnreadable(paths_.v1_backup);
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::Blocked);
  EXPECT_EQ(result.issue, TypoLearningLoadIssue::V1BackupUnreadable);
  EXPECT_FALSE(Exists(EncryptedPathFor(paths_.v2_store)));
}

TEST_F(TypoLearningStoreTest, FailedMigrationFromTheV1StoreReadsV1AsItIs) {
  WriteEncrypted(v1_, std::string(kHeader) + "こんちには\tこんにちは\t3 1700000000\n");
  crypto_.fail_encrypt = true;
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::V1Fallback);
  EXPECT_EQ(result.issue, TypoLearningLoadIssue::MigrationFailed);
  EXPECT_FALSE(Exists(EncryptedPathFor(paths_.v2_store)));
  EXPECT_TRUE(Exists(EncryptedPathFor(v1_)));

  // Observations go to the v1 store; patterns are not kept.
  crypto_.fail_encrypt = false;
  EXPECT_TRUE(store->ObserveReadingPair("こんちには", "こんにちは", kNow));
  EXPECT_EQ(store->LookupReadingPair("こんちには", 3, kNow), "こんにちは");
  EXPECT_FALSE(store
                   ->RecordPatternEvent(PatternEvent("j_insert_after_u", "j_delete_after_u",
                                                     TypoPatternOutcome::Accept, kNow))
                   .has_value());
  ASSERT_TRUE(store->Save(kNow));
  ASSERT_TRUE(Decrypted(v1_).has_value());
  EXPECT_TRUE(Contains(DecryptedText(v1_), "4 "));

  // The next start migrates again.
  auto reloaded = MakeStore();
  const auto again = reloaded->Load(kNow);
  EXPECT_EQ(again.mode, TypoLearningMode::V2);
  EXPECT_EQ(reloaded->LookupReadingPair("こんちには", 4, kNow), "こんにちは");
}

TEST_F(TypoLearningStoreTest, FailedMigrationFromTheBackupBlocksWithoutV2) {
  WriteEncrypted(paths_.v1_backup, std::string(kHeader) + "こんちには\tこんにちは\t3 1\n");
  crypto_.fail_encrypt = true;
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::Blocked);
  EXPECT_EQ(result.issue, TypoLearningLoadIssue::MigrationFailed);
  EXPECT_FALSE(Exists(EncryptedPathFor(paths_.v2_store)));
  EXPECT_FALSE(Exists(EncryptedPathFor(v1_)));
}

TEST_F(TypoLearningStoreTest, UnreadableV2BlocksAndLeavesEveryFile) {
  WriteUnreadable(paths_.v2_store);
  WriteEncrypted(v1_, std::string(kHeader) + "こんちには\tこんにちは\t3 1\n");
  const auto v2_bytes = ReadBytes(EncryptedPathFor(paths_.v2_store));
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::Blocked);
  EXPECT_EQ(result.issue, TypoLearningLoadIssue::V2Unreadable);
  EXPECT_FALSE(store->Save(kNow));
  EXPECT_FALSE(store->ResetAll(kNow));
  EXPECT_EQ(ReadBytes(EncryptedPathFor(paths_.v2_store)), v2_bytes);
  EXPECT_TRUE(Exists(EncryptedPathFor(v1_)));
}

TEST_F(TypoLearningStoreTest, CorruptV2IsCopiedAsideAndMigratedAgainFromV1) {
  WriteEncrypted(paths_.v2_store, "this is not a database file at all, only text");
  WriteEncrypted(v1_, std::string(kHeader) + "こんちには\tこんにちは\t3 1700000000\n");
  const auto corrupt_bytes = ReadBytes(EncryptedPathFor(paths_.v2_store));
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::V2);
  EXPECT_TRUE(result.rebuilt_from_corruption);
  EXPECT_EQ(result.corruption_reason, "deserialize");
  auto copy = EncryptedPathFor(paths_.v2_store);
  copy += ".corrupt-" + std::to_string(kNow);
  EXPECT_EQ(ReadBytes(copy), corrupt_bytes);
  EXPECT_EQ(store->origin(), TypoStoreOrigin::V1Migrated);
  EXPECT_EQ(store->ReadingPairs().size(), 1u);
  EXPECT_FALSE(Exists(EncryptedPathFor(v1_)));
  EXPECT_TRUE(Exists(EncryptedPathFor(paths_.v1_backup)));
}

TEST_F(TypoLearningStoreTest, EmptyTruncatedAndSchemalessV2AreCorruptAndLoseNoV1Data) {
  MakeMigratedV2();
  const auto valid = DecryptedText(paths_.v2_store);
  const struct {
    std::string bytes;
    std::string reason;
  } cases[] = {
      {"", "empty"},
      {valid.substr(0, valid.size() / 2), ""},
      {WithUserVersion(valid, 0), "schema"},
  };
  uint64_t now = kNow;
  for (const auto& c : cases) {
    SCOPED_TRACE(c.reason.empty() ? "truncated" : c.reason);
    WriteEncrypted(paths_.v2_store, c.bytes);
    const auto corrupt_bytes = ReadBytes(EncryptedPathFor(paths_.v2_store));
    // A start of its own second, so each corrupt file gets its own copy.
    ++now;
    auto store = MakeStore();
    const auto result = store->Load(now);
    EXPECT_EQ(result.mode, TypoLearningMode::V2);
    EXPECT_TRUE(result.rebuilt_from_corruption);
    if (!c.reason.empty()) {
      EXPECT_EQ(result.corruption_reason, c.reason);
    }
    auto copy = EncryptedPathFor(paths_.v2_store);
    copy += ".corrupt-" + std::to_string(now);
    EXPECT_EQ(ReadBytes(copy), corrupt_bytes);
    // The pairs come back from the v1 backup.
    EXPECT_EQ(store->LookupReadingPair("こんちには", 3, now), "こんにちは");
    EXPECT_EQ(store->origin(), TypoStoreOrigin::V1Migrated);
  }
}

TEST_F(TypoLearningStoreTest, ADifferentCopyFromTheSameSecondStopsSavingInsteadOfDroppingTheFile) {
  WriteEncrypted(paths_.v2_store, "garbage");
  const auto corrupt_bytes = ReadBytes(EncryptedPathFor(paths_.v2_store));
  auto copy = EncryptedPathFor(paths_.v2_store);
  copy += ".corrupt-" + std::to_string(kNow);
  WriteBytes(copy, "an earlier corrupt file");
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::Blocked);
  EXPECT_EQ(result.issue, TypoLearningLoadIssue::CorruptCopyFailed);
  EXPECT_EQ(ReadBytes(copy), "an earlier corrupt file");
  EXPECT_EQ(ReadBytes(EncryptedPathFor(paths_.v2_store)), corrupt_bytes);
  // The same bytes from the same second count as already copied.
  WriteBytes(copy, corrupt_bytes);
  auto again = MakeStore();
  EXPECT_EQ(again->Load(kNow).mode, TypoLearningMode::V2);
}

TEST_F(TypoLearningStoreTest, CorruptV2WithoutV1FilesIsRebuiltEmptyAndReplaced) {
  WriteEncrypted(paths_.v2_store, "garbage");
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::V2);
  EXPECT_TRUE(result.rebuilt_from_corruption);
  EXPECT_EQ(store->origin(), TypoStoreOrigin::Rebuilt);
  // The corrupt file was replaced, so the next start finds a valid store.
  auto reloaded = MakeStore();
  const auto again = reloaded->Load(kNow + 1);
  EXPECT_FALSE(again.rebuilt_from_corruption);
  EXPECT_EQ(reloaded->origin(), TypoStoreOrigin::Rebuilt);
}

TEST_F(TypoLearningStoreTest, NewerSchemaStopsSavingAndIsNeverOverwritten) {
  MakeMigratedV2();
  WriteEncrypted(paths_.v2_store, WithUserVersion(DecryptedText(paths_.v2_store), 2));
  const auto bytes = ReadBytes(EncryptedPathFor(paths_.v2_store));
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::Blocked);
  EXPECT_EQ(result.issue, TypoLearningLoadIssue::V2TooNew);
  EXPECT_FALSE(result.rebuilt_from_corruption);
  EXPECT_TRUE(store->ObserveReadingPair("きょうわ", "きょうは", kNow));
  EXPECT_FALSE(store->Save(kNow));
  EXPECT_EQ(ReadBytes(EncryptedPathFor(paths_.v2_store)), bytes);
}

TEST_F(TypoLearningStoreTest, UnmarkedV2IsMigratedAgainFromTheV1Store) {
  WriteEncrypted(v1_, std::string(kHeader) + "こんちには\tこんにちは\t3 1700000000\n");
  const auto v1_text = DecryptedText(v1_);
  {
    auto store = MakeStore();
    ASSERT_EQ(store->Load(kNow).mode, TypoLearningMode::V2);
    ASSERT_TRUE(store->ObserveReadingPair("きょうわ", "きょうは", kNow));
    ASSERT_TRUE(store->Save(kNow));
  }
  // Drop the mark, and put the v1 store back as if step 4 had never run.
  WriteEncrypted(paths_.v2_store,
                 ExecOnDatabase(DecryptedText(paths_.v2_store), "DELETE FROM typo_settings"));
  std::filesystem::remove(EncryptedPathFor(paths_.v1_backup));
  WriteEncrypted(v1_, v1_text);

  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::V2);
  EXPECT_TRUE(result.migrated);
  EXPECT_EQ(store->origin(), TypoStoreOrigin::V1Migrated);
  // Only the v1 pairs: the unverified v2 was discarded.
  EXPECT_EQ(store->ReadingPairs().size(), 1u);
}

TEST_F(TypoLearningStoreTest, UnmarkedV2WithoutV1FilesIsRebuiltAndWithUnreadableV1Blocks) {
  MakeMigratedV2();
  const auto unmarked = ExecOnDatabase(DecryptedText(paths_.v2_store), "DELETE FROM typo_settings");

  // The backup is migrated again.
  WriteEncrypted(paths_.v2_store, unmarked);
  {
    auto store = MakeStore();
    const auto result = store->Load(kNow);
    EXPECT_EQ(result.mode, TypoLearningMode::V2);
    EXPECT_EQ(store->origin(), TypoStoreOrigin::V1Migrated);
  }
  // Without any v1 file, an empty store is rebuilt.
  std::filesystem::remove(EncryptedPathFor(paths_.v1_backup));
  WriteEncrypted(paths_.v2_store, unmarked);
  {
    auto store = MakeStore();
    EXPECT_EQ(store->Load(kNow).mode, TypoLearningMode::V2);
    EXPECT_EQ(store->origin(), TypoStoreOrigin::Rebuilt);
    EXPECT_TRUE(store->ReadingPairs().empty());
  }
  // An unreadable v1 store leaves both sides alone and blocks.
  WriteEncrypted(paths_.v2_store, unmarked);
  WriteUnreadable(v1_);
  const auto v2_bytes = ReadBytes(EncryptedPathFor(paths_.v2_store));
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::Blocked);
  EXPECT_EQ(result.issue, TypoLearningLoadIssue::V1StoreUnreadable);
  EXPECT_EQ(ReadBytes(EncryptedPathFor(paths_.v2_store)), v2_bytes);
}

TEST_F(TypoLearningStoreTest, LeftoverV1StoreIsFoldedInByColumnMaximum) {
  MakeMigratedV2();
  {
    auto store = MakeStore();
    store->Load(kNow);
    for (int i = 0; i < 2; ++i) store->ObserveReadingPair("こんちには", "こんにちは", kNow + 10);
    ASSERT_TRUE(store->Save(kNow + 10));
  }
  // An older Host wrote the v1 store again (count 3 again, and a new pair).
  WriteEncrypted(v1_, std::string(kHeader) + "こんちには\tこんにちは\t3 1700000000\n" +
                          "きょうわ\tきょうは\t2 1700000005\n");
  auto store = MakeStore();
  const auto result = store->Load(kNow + 20);
  EXPECT_EQ(result.mode, TypoLearningMode::V2);
  EXPECT_FALSE(result.v1_store_pending);
  const auto pairs = store->ReadingPairs();
  ASSERT_EQ(pairs.size(), 2u);
  EXPECT_EQ(pairs[1].wrong_reading, "こんちには");
  EXPECT_EQ(pairs[1].record.count, 5u);  // max(5, 3), not 8
  EXPECT_EQ(pairs[1].record.last_updated_epoch_sec, kNow + 10);
  EXPECT_EQ(pairs[0].record.count, 2u);
  // The v1 store was appended to the existing backup.
  EXPECT_FALSE(Exists(EncryptedPathFor(v1_)));
  EXPECT_TRUE(Contains(DecryptedText(paths_.v1_backup), "きょうわ"));
}

TEST_F(TypoLearningStoreTest, AppendingToTheBackupKeepsUnparsableRowsAndEndsLines) {
  const std::string backup =
      std::string(kHeader) + "this row is not parsable\nかいぎ\tかいしゃ\t2 5";
  WriteEncrypted(paths_.v1_backup, backup);
  const std::string v1 = std::string(kHeader) + "こんちには\tこんにちは\t3 1700000000\n";
  WriteEncrypted(v1_, v1);
  auto store = MakeStore();
  ASSERT_EQ(store->Load(kNow).mode, TypoLearningMode::V2);
  EXPECT_EQ(DecryptedText(paths_.v1_backup), backup + "\n" + v1);
  // The v1 parser reads the combined backup with the v1 store's values.
  TypoCorrectionStore parsed(paths_.v1_backup, &crypto_);
  parsed.LoadText(DecryptedText(paths_.v1_backup));
  EXPECT_EQ(parsed.size(), 2u);
}

TEST_F(TypoLearningStoreTest, UnreadableBackupLeavesTheV1StoreAndFailsReadingPairForget) {
  WriteUnreadable(paths_.v1_backup);
  WriteEncrypted(v1_, std::string(kHeader) + "こんちには\tこんにちは\t3 1700000000\n");
  // The v1 store exists, so migration still runs; step 5 cannot.
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::V2);
  EXPECT_TRUE(result.v1_store_pending);
  EXPECT_TRUE(result.v1_side_unreadable);
  EXPECT_TRUE(Exists(EncryptedPathFor(v1_)));
  EXPECT_EQ(store->ForgetReadingPair("こんちには", "こんにちは", kNow), TypoForgetResult::Failed);
  EXPECT_EQ(store->LookupReadingPair("こんちには", 3, kNow), "こんにちは");
  // The reset of the whole store deletes without reading.
  EXPECT_TRUE(store->ResetAll(kNow));
  EXPECT_FALSE(Exists(EncryptedPathFor(paths_.v1_backup)));
  EXPECT_FALSE(Exists(EncryptedPathFor(v1_)));
  EXPECT_TRUE(store->ReadingPairs().empty());
}

TEST_F(TypoLearningStoreTest, ReadingPairsFollowTheV1Rules) {
  auto store = MakeStore();
  store->Load(kNow);
  EXPECT_FALSE(store->ObserveReadingPair("あいうえお", "あいうえお", kNow));
  EXPECT_FALSE(store->ObserveReadingPair("あ", "い", kNow));
  for (int i = 0; i < 3; ++i) store->ObserveReadingPair("かんじへんかん", "かんじへんこう", kNow);
  for (int i = 0; i < 3; ++i) store->ObserveReadingPair("かんじへんかん", "かんじへんかい", kNow);
  EXPECT_FALSE(store->LookupReadingPair("かんじへんかん", 4, kNow).has_value());
  // Ties go to the smaller reading.
  EXPECT_EQ(store->LookupReadingPair("かんじへんかん", 3, kNow), "かんじへんかい");
  // Records older than the v1 cutoff are ignored.
  EXPECT_FALSE(store->LookupReadingPair("かんじへんかん", 3, kNow + 181 * kDay).has_value());
  EXPECT_TRUE(store->LookupReadingPair("かんじへんかん", 3, kNow + 179 * kDay).has_value());
}

TEST_F(TypoLearningStoreTest, SavedStoreRoundTrips) {
  auto store = MakeStore();
  store->Load(kNow);
  for (int i = 0; i < 3; ++i) store->ObserveReadingPair("こんちには", "こんにちは", kNow);
  const auto id = store->RecordPatternEvent(PatternEvent(
      "j_insert_after_u", "j_delete_after_u", TypoPatternOutcome::Accept, kNow, "notepad.exe"));
  ASSERT_TRUE(id.has_value());
  ASSERT_TRUE(store->Save(kNow));

  auto reloaded = MakeStore();
  ASSERT_EQ(reloaded->Load(kNow).mode, TypoLearningMode::V2);
  EXPECT_EQ(reloaded->LookupReadingPair("こんちには", 3, kNow), "こんにちは");
  const auto patterns = reloaded->Patterns();
  ASSERT_EQ(patterns.size(), 1u);
  EXPECT_EQ(patterns[0].id, *id);
  EXPECT_EQ(patterns[0].accept_count, 1u);
  EXPECT_EQ(reloaded->EventCount(), 1u);
  EXPECT_EQ(reloaded->DominantApp(*id), "notepad.exe");
  // Nothing on disk is plaintext.
  EXPECT_FALSE(Contains(ReadBytes(EncryptedPathFor(paths_.v2_store)), "こんちには"));
  EXPECT_FALSE(Exists(paths_.v2_store));
}

TEST_F(TypoLearningStoreTest, RetentionLimitsApplyBeforeSaving) {
  auto store = MakeStore(TypoLearningLimits{100, 2});
  store->Load(kNow);
  store->ObserveReadingPair("こんちには", "こんにちは", kNow - 200);
  store->ObserveReadingPair("きょうわ", "きょうは", kNow - 10);
  store->ObserveReadingPair("ありがと", "ありがとう", kNow - 5);
  store->ObserveReadingPair("かいしや", "かいしゃ", kNow);
  for (uint64_t t : {kNow - 300, kNow - 3, kNow - 2, kNow - 1}) {
    store->RecordPatternEvent(
        PatternEvent("j_insert_after_u", "j_delete_after_u", TypoPatternOutcome::Accept, t));
  }
  ASSERT_TRUE(store->Save(kNow));
  const auto pairs = store->ReadingPairs();
  ASSERT_EQ(pairs.size(), 2u);
  for (const auto& pair : pairs) EXPECT_NE(pair.wrong_reading, "きょうわ");
  EXPECT_EQ(store->EventCount(), 2u);
  // Patterns are not trimmed.
  EXPECT_EQ(store->Patterns().size(), 1u);
}

TEST_F(TypoLearningStoreTest, SuspendedWritesNeitherLearnNorTouchFiles) {
  MakeMigratedV2();
  auto store = MakeStore();
  ASSERT_EQ(store->Load(kNow).mode, TypoLearningMode::V2);
  const auto v2_bytes = ReadBytes(EncryptedPathFor(paths_.v2_store));
  const auto backup_bytes = ReadBytes(EncryptedPathFor(paths_.v1_backup));

  store->SetWritesSuspended(true);
  EXPECT_FALSE(store->ObserveReadingPair("きょうわ", "きょうは", kNow));
  EXPECT_FALSE(store
                   ->RecordPatternEvent(PatternEvent("j_insert_after_u", "j_delete_after_u",
                                                     TypoPatternOutcome::Accept, kNow))
                   .has_value());
  EXPECT_FALSE(store->Save(kNow + 1));
  EXPECT_EQ(store->ForgetReadingPair("こんちには", "こんにちは", kNow), TypoForgetResult::Failed);
  EXPECT_EQ(store->ForgetPattern("j_insert_after_u", "j_delete_after_u", kNow),
            TypoForgetResult::Failed);
  EXPECT_FALSE(store->ResetAll(kNow));
  EXPECT_EQ(ReadBytes(EncryptedPathFor(paths_.v2_store)), v2_bytes);
  EXPECT_EQ(ReadBytes(EncryptedPathFor(paths_.v1_backup)), backup_bytes);
  EXPECT_FALSE(store->LookupReadingPair("きょうわ", 1, kNow).has_value());
  EXPECT_TRUE(store->Patterns().empty());

  store->SetWritesSuspended(false);
  EXPECT_TRUE(store->ObserveReadingPair("きょうわ", "きょうは", kNow));
  EXPECT_TRUE(store->Save(kNow + 1));
}

TEST_F(TypoLearningStoreTest, PatternsAcceptOnlyAbstractionTokens) {
  auto store = MakeStore();
  store->Load(kNow);
  // Raw keys, readings and malformed tokens are refused.
  for (const char* raw :
       {"kousyou", "こうしょう", "j_insert_after_", "j_insert_after_uu", "a_sub_^", "1_sub_2",
        "A_sub_b", "a_sub_b+", "a_sub_b+c_sub_d+e_sub_f+g_sub_h"}) {
    SCOPED_TRACE(raw);
    EXPECT_FALSE(store
                     ->RecordPatternEvent(
                         PatternEvent(raw, "j_delete_after_u", TypoPatternOutcome::Accept, kNow))
                     .has_value());
    EXPECT_FALSE(store
                     ->RecordPatternEvent(
                         PatternEvent("j_insert_after_u", raw, TypoPatternOutcome::Accept, kNow))
                     .has_value());
  }
  auto unknown_type = PatternEvent("z_sub_x", "x_sub_z", TypoPatternOutcome::Accept, kNow);
  unknown_type.typo_type = "keyboard_smash";
  EXPECT_FALSE(store->RecordPatternEvent(unknown_type).has_value());
  auto bad_hash = PatternEvent("z_sub_x", "x_sub_z", TypoPatternOutcome::Accept, kNow);
  bad_hash.left_context_hash = "raw left context";
  EXPECT_FALSE(store->RecordPatternEvent(bad_hash).has_value());
  EXPECT_TRUE(store->Patterns().empty());
  EXPECT_EQ(store->EventCount(), 0u);

  EXPECT_TRUE(store
                  ->RecordPatternEvent(PatternEvent("ts_swap+ん_delete_after_^", "st_swap",
                                                    TypoPatternOutcome::Accept, kNow))
                  .has_value());
}

TEST_F(TypoLearningStoreTest, PatternCountsConfidenceAndDominantApp) {
  auto store = MakeStore();
  store->Load(kNow);
  const auto accept = [&](uint64_t t, const char* app) {
    return store->RecordPatternEvent(
        PatternEvent("j_insert_after_u", "j_delete_after_u", TypoPatternOutcome::Accept, t, app));
  };
  const auto id = accept(kNow, "notepad.exe");
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(accept(kNow + 1, "notepad.exe"), id);
  EXPECT_EQ(
      store->RecordPatternEvent(PatternEvent("j_insert_after_u", "j_delete_after_u",
                                             TypoPatternOutcome::Reject, kNow + 2, "code.exe")),
      id);
  const auto record = store->Patterns().front();
  EXPECT_EQ(record.accept_count, 2u);
  EXPECT_EQ(record.reject_count, 1u);
  // A rejection does not count as use.
  EXPECT_EQ(record.last_used_at, kNow + 1);
  EXPECT_EQ(record.created_at, kNow);
  EXPECT_EQ(record.updated_at, kNow + 2);
  EXPECT_DOUBLE_EQ(record.confidence, ComputeTypoConfidence(2, 1, kNow + 1, kNow + 2, false));

  EXPECT_EQ(store->DominantApp(*id), "notepad.exe");
  EXPECT_DOUBLE_EQ(*store->PatternConfidence(*id, "notepad.exe", true, kNow + 2),
                   ComputeTypoConfidence(2, 1, kNow + 1, kNow + 2, true));
  EXPECT_DOUBLE_EQ(*store->PatternConfidence(*id, "notepad.exe", false, kNow + 2),
                   ComputeTypoConfidence(2, 1, kNow + 1, kNow + 2, false));
  EXPECT_DOUBLE_EQ(*store->PatternConfidence(*id, "code.exe", true, kNow + 2),
                   ComputeTypoConfidence(2, 1, kNow + 1, kNow + 2, false));
  EXPECT_FALSE(store->PatternConfidence(*id + 100, "", false, kNow).has_value());

  // A tie between apps gives no dominant app.
  accept(kNow + 3, "code.exe");
  accept(kNow + 4, "code.exe");
  EXPECT_FALSE(store->DominantApp(*id).has_value());
}

TEST(TypoConfidenceTest, MatchesTheSection129FixedPoints) {
  constexpr uint64_t now = 1'700'000'000;
  // A new pattern starts neutral.
  EXPECT_DOUBLE_EQ(ComputeTypoConfidence(0, 0, std::nullopt, now, false), 0.5);
  // Two accepts with both bonuses: logit 1.0, about 0.73.
  EXPECT_NEAR(ComputeTypoConfidence(2, 0, now, now, true), 1.0 / (1.0 + std::exp(-1.0)), 1e-12);
  // At the half-life the recency bonus is neutral.
  EXPECT_NEAR(ComputeTypoConfidence(0, 0, now - 60 * 86400, now, false), 0.5, 1e-12);
  // Long idle approaches -B_rec.
  EXPECT_NEAR(ComputeTypoConfidence(0, 0, now - 6000 * 86400, now, false),
              1.0 / (1.0 + std::exp(0.30)), 1e-6);
  // Clamped to [0.05, 0.95].
  EXPECT_DOUBLE_EQ(ComputeTypoConfidence(100, 0, now, now, true), 0.95);
  EXPECT_DOUBLE_EQ(ComputeTypoConfidence(0, 100, std::nullopt, now, false), 0.05);
  // A rejection outweighs an acceptance.
  EXPECT_LT(ComputeTypoConfidence(1, 1, std::nullopt, now, false), 0.5);
  // A future timestamp counts as used now.
  EXPECT_DOUBLE_EQ(ComputeTypoConfidence(0, 0, now + 100, now, false),
                   ComputeTypoConfidence(0, 0, now, now, false));
}

TEST(TypoPatternTest, AbstractsEditsIntoTokens) {
  using azookey::learning::AbstractTypoEdits;
  using azookey::learning::IsValidTypoPattern;
  using azookey::learning::TypoEdit;
  using azookey::learning::TypoEditKind;
  EXPECT_EQ(AbstractTypoEdits({TypoEdit{TypoEditKind::Insertion, U'j', 0, U'u'}}),
            "j_insert_after_u");
  EXPECT_EQ(AbstractTypoEdits({TypoEdit{TypoEditKind::Deletion, U'x', 0, U'^'}}),
            "x_delete_after_^");
  EXPECT_EQ(AbstractTypoEdits({TypoEdit{TypoEditKind::Substitution, U'z', U'x', 0},
                               TypoEdit{TypoEditKind::Transposition, U't', U's', 0}}),
            "z_sub_x+ts_swap");
  EXPECT_EQ(AbstractTypoEdits({TypoEdit{TypoEditKind::Substitution, U'し', U'じ', 0}}),
            "し_sub_じ");
  // Keys outside the alphabet are discarded, not abstracted.
  EXPECT_FALSE(AbstractTypoEdits({TypoEdit{TypoEditKind::Substitution, U'1', U'2', 0}}));
  EXPECT_FALSE(AbstractTypoEdits({TypoEdit{TypoEditKind::Insertion, U'A', 0, U'u'}}));
  EXPECT_FALSE(AbstractTypoEdits({TypoEdit{TypoEditKind::Insertion, U'j', 0, U'/'}}));
  EXPECT_FALSE(AbstractTypoEdits({}));
  EXPECT_FALSE(AbstractTypoEdits(
      std::vector<TypoEdit>(4, TypoEdit{TypoEditKind::Substitution, U'a', U'b', 0})));
  EXPECT_TRUE(IsValidTypoPattern("ab_swap"));
  EXPECT_TRUE(IsValidTypoPattern("'_insert_after_-"));
  EXPECT_TRUE(IsValidTypoPattern("ー_sub_う"));
  EXPECT_FALSE(IsValidTypoPattern(""));
  EXPECT_FALSE(IsValidTypoPattern("+"));
  EXPECT_FALSE(IsValidTypoPattern("abc_swap"));
  EXPECT_FALSE(IsValidTypoPattern("a_swap"));
  EXPECT_FALSE(IsValidTypoPattern("a_sub_b c"));
}

TEST_F(TypoLearningStoreTest, ForgettingAPairRemovesItFromV2AndTheV1Side) {
  WriteEncrypted(v1_, std::string(kHeader) + "こんちには\tこんにちは\t3 1700000000\n" +
                          "きょうわ\tきょうは\t3 1700000000\n");
  auto store = MakeStore();
  ASSERT_EQ(store->Load(kNow).mode, TypoLearningMode::V2);
  auto event =
      PatternEvent("j_insert_after_u", "j_delete_after_u", TypoPatternOutcome::Accept, kNow);
  event.observed_reading = "こんちには";
  event.corrected_reading = "こんにちは";
  ASSERT_TRUE(store->RecordPatternEvent(event).has_value());
  ASSERT_TRUE(store->Save(kNow));
  // An unparsable row in the backup must survive the rewrite.
  WriteEncrypted(paths_.v1_backup, DecryptedText(paths_.v1_backup) + "not a row\n");

  EXPECT_EQ(store->ForgetReadingPair("こんちには", "こんにちは", kNow), TypoForgetResult::Removed);
  EXPECT_FALSE(store->LookupReadingPair("こんちには", 1, kNow).has_value());
  EXPECT_EQ(store->EventCount(), 0u);
  const auto backup = DecryptedText(paths_.v1_backup);
  EXPECT_FALSE(Contains(backup, "こんちには"));
  EXPECT_TRUE(Contains(backup, "きょうわ"));
  EXPECT_TRUE(Contains(backup, "not a row\n"));
  // secure_delete and VACUUM leave no copy of the reading in the saved bytes.
  EXPECT_FALSE(Contains(DecryptedText(paths_.v2_store), "こんちには"));
  EXPECT_TRUE(Contains(DecryptedText(paths_.v2_store), "きょうわ"));

  EXPECT_EQ(store->ForgetReadingPair("こんちには", "こんにちは", kNow), TypoForgetResult::NotFound);
}

TEST_F(TypoLearningStoreTest, ForgettingAPatternRemovesItsEvents) {
  auto store = MakeStore();
  store->Load(kNow);
  store->RecordPatternEvent(
      PatternEvent("j_insert_after_u", "j_delete_after_u", TypoPatternOutcome::Accept, kNow));
  store->RecordPatternEvent(PatternEvent("z_sub_x", "x_sub_z", TypoPatternOutcome::Accept, kNow));
  ASSERT_TRUE(store->Save(kNow));
  EXPECT_EQ(store->ForgetPattern("j_insert_after_u", "j_delete_after_u", kNow),
            TypoForgetResult::Removed);
  ASSERT_EQ(store->Patterns().size(), 1u);
  EXPECT_EQ(store->EventCount(), 1u);
  EXPECT_FALSE(Contains(DecryptedText(paths_.v2_store), "j_insert_after_u"));
  EXPECT_EQ(store->ForgetPattern("j_insert_after_u", "j_delete_after_u", kNow),
            TypoForgetResult::NotFound);
}

TEST_F(TypoLearningStoreTest, ResetRemovesV1FilesAndLearningButKeepsSettings) {
  WriteEncrypted(v1_, std::string(kHeader) + "こんちには\tこんにちは\t3 1700000000\n");
  WriteEncrypted(paths_.v1_backup, std::string(kHeader) + "きょうわ\tきょうは\t3 1700000000\n");
  auto store = MakeStore();
  ASSERT_EQ(store->Load(kNow).mode, TypoLearningMode::V2);
  store->RecordPatternEvent(PatternEvent("z_sub_x", "x_sub_z", TypoPatternOutcome::Accept, kNow));
  ASSERT_TRUE(store->ResetAll(kNow));
  EXPECT_FALSE(Exists(EncryptedPathFor(paths_.v1_backup)));
  EXPECT_FALSE(Exists(EncryptedPathFor(v1_)));
  EXPECT_TRUE(store->ReadingPairs().empty());
  EXPECT_TRUE(store->Patterns().empty());
  EXPECT_EQ(store->origin(), TypoStoreOrigin::V1Migrated);
  const auto saved = DecryptedText(paths_.v2_store);
  EXPECT_FALSE(Contains(saved, "こんちには"));
  EXPECT_FALSE(Contains(saved, "z_sub_x"));
}

TEST_F(TypoLearningStoreTest, FailedSaveKeepsThePublishedContents) {
  MakeMigratedV2();
  auto store = MakeStore();
  ASSERT_EQ(store->Load(kNow).mode, TypoLearningMode::V2);
  store->RecordPatternEvent(PatternEvent("z_sub_x", "x_sub_z", TypoPatternOutcome::Accept, kNow));
  crypto_.fail_encrypt = true;
  EXPECT_EQ(store->ForgetReadingPair("こんちには", "こんにちは", kNow), TypoForgetResult::Failed);
  EXPECT_EQ(store->LookupReadingPair("こんちには", 3, kNow), "こんにちは");
  EXPECT_EQ(store->ForgetPattern("z_sub_x", "x_sub_z", kNow), TypoForgetResult::Failed);
  EXPECT_EQ(store->Patterns().size(), 1u);
  EXPECT_FALSE(store->ResetAll(kNow));
  EXPECT_EQ(store->LookupReadingPair("こんちには", 3, kNow), "こんにちは");
  EXPECT_EQ(store->Patterns().size(), 1u);
}

TEST_F(TypoLearningStoreTest, V1FallbackForgetsAndResetsTheBackupFirst) {
  WriteEncrypted(v1_, std::string(kHeader) + "こんちには\tこんにちは\t3 1700000000\n");
  WriteEncrypted(paths_.v1_backup, std::string(kHeader) + "こんちには\tこんにちは\t1 1\n" +
                                       "きょうわ\tきょうは\t1 1\n");
  crypto_.fail_encrypt = true;
  auto store = MakeStore();
  ASSERT_EQ(store->Load(kNow).mode, TypoLearningMode::V1Fallback);
  crypto_.fail_encrypt = false;
  EXPECT_EQ(store->ForgetReadingPair("こんちには", "こんにちは", kNow), TypoForgetResult::Removed);
  EXPECT_FALSE(Contains(DecryptedText(paths_.v1_backup), "こんちには"));
  EXPECT_FALSE(Contains(DecryptedText(v1_), "こんちには"));
  EXPECT_TRUE(store->ResetAll(kNow));
  EXPECT_FALSE(Exists(EncryptedPathFor(paths_.v1_backup)));
  EXPECT_TRUE(store->ReadingPairs().empty());
}

TEST_F(TypoLearningStoreTest, BackupItemsRoundTripAndMerge) {
  auto store = MakeStore();
  store->Load(kNow);
  for (int i = 0; i < 3; ++i) store->ObserveReadingPair("こんちには", "こんにちは", kNow);
  store->RecordPatternEvent(
      PatternEvent("j_insert_after_u", "j_delete_after_u", TypoPatternOutcome::Accept, kNow));
  store->RecordPatternEvent(
      PatternEvent("j_insert_after_u", "j_delete_after_u", TypoPatternOutcome::Reject, kNow + 1));

  // typo_corrections stays the v1 TSV, readable by the v1 parser.
  TypoCorrectionStore pairs(std::filesystem::path{}, &crypto_);
  ASSERT_TRUE(pairs.LoadText(store->SerializeReadingPairsText()));
  ASSERT_EQ(pairs.size(), 1u);
  EXPECT_EQ(pairs.All()[0].record.count, 3u);

  std::vector<TypoPatternRecord> patterns;
  ASSERT_TRUE(TypoLearningStore::ParsePatternsText(store->SerializePatternsText(), patterns));
  ASSERT_EQ(patterns.size(), 1u);
  EXPECT_EQ(patterns[0].accept_count, 1u);
  EXPECT_EQ(patterns[0].reject_count, 1u);
  EXPECT_EQ(patterns[0].last_used_at, kNow);

  // Merge adds counts and keeps the later timestamps; the confidence is recomputed.
  auto target =
      std::make_unique<TypoLearningStore>(dir_ / "other" / "typo_corrections.tsv", &crypto_);
  std::filesystem::create_directories(dir_ / "other");
  target->Load(kNow);
  target->ObserveReadingPair("こんちには", "こんにちは", kNow + 5);
  target->RecordPatternEvent(
      PatternEvent("j_insert_after_u", "j_delete_after_u", TypoPatternOutcome::Accept, kNow + 9));
  const auto pair_counts = target->MergeReadingPairs(pairs, ImportConflictPolicy::Merge);
  ASSERT_TRUE(pair_counts.has_value());
  EXPECT_EQ(pair_counts->conflicts, 1u);
  EXPECT_EQ(pair_counts->imported, 1u);
  EXPECT_EQ(target->ReadingPairs()[0].record.count, 4u);
  EXPECT_EQ(target->ReadingPairs()[0].record.last_updated_epoch_sec, kNow + 5);
  const auto pattern_counts =
      target->MergePatterns(patterns, ImportConflictPolicy::Merge, kNow + 10);
  ASSERT_TRUE(pattern_counts.has_value());
  EXPECT_EQ(pattern_counts->conflicts, 1u);
  const auto merged = target->Patterns().front();
  EXPECT_EQ(merged.accept_count, 2u);
  EXPECT_EQ(merged.reject_count, 1u);
  EXPECT_EQ(merged.last_used_at, kNow + 9);
  EXPECT_EQ(merged.created_at, kNow);
  EXPECT_DOUBLE_EQ(merged.confidence, ComputeTypoConfidence(2, 1, kNow + 9, kNow + 10, false));

  EXPECT_EQ(target->MergePatterns(patterns, ImportConflictPolicy::KeepBoth, kNow)->skipped, 1u);
  ASSERT_TRUE(target->MergePatterns(patterns, ImportConflictPolicy::Overwrite, kNow + 10));
  EXPECT_EQ(target->Patterns().front().accept_count, 1u);
}

TEST(TypoPatternsTextTest, MalformedRowsAndAMissingHeaderAreReported) {
  std::vector<TypoPatternRecord> patterns;
  const std::string header = "# azookey-typo-patterns-tsv escaped=1\n";
  ASSERT_FALSE(TypoLearningStore::ParsePatternsText(
      header + "j_insert_after_u\tj_delete_after_u\textra_char\t\t1\t0\t\t5\t5\n"
               "kousyou\tkousho\textra_char\t\t1\t0\t\t5\t5\n"
               "z_sub_x\tx_sub_z\textra_char\t\tmany\t0\t\t5\t5\n",
      patterns));
  ASSERT_EQ(patterns.size(), 1u);
  EXPECT_FALSE(patterns[0].last_used_at.has_value());
  EXPECT_FALSE(TypoLearningStore::ParsePatternsText(
      "j_insert_after_u\tj_delete_after_u\textra_char\t\t1\t0\t\t5\t5\n", patterns));
  EXPECT_TRUE(patterns.empty());
}

TEST(TypoCorrectionStoreRowsTest, WithoutPairRowsKeepsEveryOtherByte) {
  const std::string text = std::string(kHeader) + "こんちには\tこんにちは\t3 1\nbroken row\n" +
                           "# azookey-typo-correction-tsv escaped=1\n" +
                           "こんちには\tこんにちは\t1 2\nきょうわ\tきょうは\t1 2";
  bool removed = false;
  const auto kept = TypoCorrectionStore::WithoutPairRows(text, "こんちには", "こんにちは", removed);
  EXPECT_TRUE(removed);
  EXPECT_EQ(kept, std::string(kHeader) + "broken row\n# azookey-typo-correction-tsv escaped=1\n" +
                      "きょうわ\tきょうは\t1 2");
  TypoCorrectionStore store(std::filesystem::path{});
  EXPECT_FALSE(store.Put(TypoCorrectionEntry{"あ", "い", TypoCorrectionRecord{1, 1}}));
  EXPECT_FALSE(store.Put(TypoCorrectionEntry{"きょうわ", "きょうは", TypoCorrectionRecord{0, 1}}));
  EXPECT_TRUE(store.Put(TypoCorrectionEntry{"きょうわ", "きょうは", TypoCorrectionRecord{2, 1}}));
}

TEST_F(TypoLearningStoreTest, AReloadedStoreForgetsAndResetsOnItsDeserializedDatabase) {
  {
    auto store = MakeStore();
    store->Load(kNow);
    for (int i = 0; i < 3; ++i) store->ObserveReadingPair("こんちには", "こんにちは", kNow);
    store->ObserveReadingPair("きょうわ", "きょうは", kNow);
    store->RecordPatternEvent(
        PatternEvent("j_insert_after_u", "j_delete_after_u", TypoPatternOutcome::Accept, kNow));
    store->RecordPatternEvent(PatternEvent("z_sub_x", "x_sub_z", TypoPatternOutcome::Accept, kNow));
    ASSERT_TRUE(store->Save(kNow));
  }
  // A second start opens the saved bytes with sqlite3_deserialize.
  auto store = MakeStore();
  ASSERT_EQ(store->Load(kNow + 1).mode, TypoLearningMode::V2);
  EXPECT_EQ(store->ForgetReadingPair("こんちには", "こんにちは", kNow + 1),
            TypoForgetResult::Removed);
  EXPECT_EQ(store->ForgetPattern("j_insert_after_u", "j_delete_after_u", kNow + 1),
            TypoForgetResult::Removed);
  auto saved = DecryptedText(paths_.v2_store);
  EXPECT_FALSE(Contains(saved, "こんちには"));
  EXPECT_FALSE(Contains(saved, "j_insert_after_u"));
  EXPECT_TRUE(Contains(saved, "きょうわ"));

  auto reloaded = MakeStore();
  ASSERT_EQ(reloaded->Load(kNow + 2).mode, TypoLearningMode::V2);
  EXPECT_FALSE(reloaded->LookupReadingPair("こんちには", 1, kNow + 2).has_value());
  EXPECT_EQ(reloaded->Patterns().size(), 1u);
  ASSERT_TRUE(reloaded->ResetAll(kNow + 2));
  saved = DecryptedText(paths_.v2_store);
  EXPECT_FALSE(Contains(saved, "きょうわ"));
  EXPECT_FALSE(Contains(saved, "z_sub_x"));
  EXPECT_TRUE(reloaded->ReadingPairs().empty());
}

TEST_F(TypoLearningStoreTest, AnUnreadableV1StoreFailsTheForgetBeforeTheBackupIsRewritten) {
  MakeMigratedV2();
  WriteUnreadable(v1_);
  const auto backup_bytes = ReadBytes(EncryptedPathFor(paths_.v1_backup));
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::V2);
  EXPECT_TRUE(result.v1_side_unreadable);
  EXPECT_EQ(store->ForgetReadingPair("こんちには", "こんにちは", kNow), TypoForgetResult::Failed);
  EXPECT_EQ(ReadBytes(EncryptedPathFor(paths_.v1_backup)), backup_bytes);
  EXPECT_EQ(store->LookupReadingPair("こんちには", 3, kNow), "こんにちは");
}

TEST_F(TypoLearningStoreTest, AnUnreadableBackupAloneIsReportedAsAnUnreadableV1Side) {
  MakeMigratedV2();
  WriteUnreadable(paths_.v1_backup);
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::V2);
  EXPECT_FALSE(result.v1_store_pending);
  EXPECT_TRUE(result.v1_side_unreadable);
  EXPECT_EQ(store->ForgetReadingPair("こんちには", "こんにちは", kNow), TypoForgetResult::Failed);
}

TEST_F(TypoLearningStoreTest, SuspendedWritesTouchNothingWhileReadingV1AsItIs) {
  WriteEncrypted(v1_, std::string(kHeader) + "こんちには\tこんにちは\t3 1700000000\n");
  crypto_.fail_encrypt = true;
  auto store = MakeStore();
  ASSERT_EQ(store->Load(kNow).mode, TypoLearningMode::V1Fallback);
  crypto_.fail_encrypt = false;
  const auto v1_bytes = ReadBytes(EncryptedPathFor(v1_));
  store->SetWritesSuspended(true);
  EXPECT_FALSE(store->ObserveReadingPair("こんちには", "こんにちは", kNow));
  EXPECT_FALSE(store->Save(kNow));
  EXPECT_EQ(store->ForgetReadingPair("こんちには", "こんにちは", kNow), TypoForgetResult::Failed);
  EXPECT_FALSE(store->ResetAll(kNow));
  EXPECT_EQ(ReadBytes(EncryptedPathFor(v1_)), v1_bytes);
  EXPECT_EQ(store->LookupReadingPair("こんちには", 3, kNow), "こんにちは");
  EXPECT_FALSE(store->LookupReadingPair("こんちには", 4, kNow).has_value());
}

TEST_F(TypoLearningStoreTest, AFailedReadBackRemovesTheUnmarkedV2AndReadsV1AsItIs) {
  WriteEncrypted(v1_, std::string(kHeader) + "こんちには\tこんにちは\t3 1700000000\n");
  // Step 2 writes the v2 store; step 3 cannot read it back.
  crypto_.fail_database_decrypt = true;
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::V1Fallback);
  EXPECT_FALSE(Exists(EncryptedPathFor(paths_.v2_store)));
  EXPECT_TRUE(Exists(EncryptedPathFor(v1_)));
  EXPECT_FALSE(Exists(EncryptedPathFor(paths_.v1_backup)));
  EXPECT_EQ(store->LookupReadingPair("こんちには", 3, kNow), "こんにちは");
}

TEST_F(TypoLearningStoreTest, AnImportOfManyDistinctPatternsStopsAtThePatternLimit) {
  auto store = MakeStore(TypoLearningLimits{180 * kDay, 10000, 50});
  store->Load(kNow);
  // Distinct two-token keys "<a>_sub_<b>+<c>_sub_z".
  const std::string letters = "abcdefghijklmnopqrstuvwxyz";
  std::vector<TypoPatternRecord> patterns;
  for (size_t i = 0; i < 5000; ++i) {
    TypoPatternRecord record;
    record.observed_pattern = std::string(1, letters[i % 26]) + "_sub_" + letters[(i / 26) % 26] +
                              "+" + letters[(i / 676) % 26] + "_sub_z";
    record.intended_pattern = "x_sub_y";
    record.typo_type = "adjacent_key";
    record.accept_count = 1;
    record.created_at = kNow;
    record.updated_at = kNow;
    patterns.push_back(record);
  }
  const auto counts = store->MergePatterns(patterns, ImportConflictPolicy::Merge, kNow);
  ASSERT_TRUE(counts.has_value());
  EXPECT_EQ(counts->imported, 50u);
  EXPECT_EQ(counts->skipped, 4950u);
  EXPECT_EQ(store->Patterns().size(), 50u);
  // Keys already in the table still merge once the limit is reached.
  const std::vector<TypoPatternRecord> first(patterns.begin(), patterns.begin() + 10);
  const auto again = store->MergePatterns(first, ImportConflictPolicy::Merge, kNow);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again->conflicts, 10u);
  EXPECT_EQ(again->imported, 10u);
  EXPECT_EQ(store->Patterns().size(), 50u);
}

TEST_F(TypoLearningStoreTest, PatternKeysAreUniqueWithANullKeyboardLayoutAsEmpty) {
  auto store = MakeStore();
  store->Load(kNow);
  const auto id = store->RecordPatternEvent(
      PatternEvent("j_insert_after_u", "j_delete_after_u", TypoPatternOutcome::Accept, kNow));
  ASSERT_TRUE(id.has_value());
  TypoPatternRecord same;
  same.observed_pattern = "j_insert_after_u";
  same.intended_pattern = "j_delete_after_u";
  same.typo_type = "extra_char";
  same.accept_count = 1;
  same.created_at = kNow;
  same.updated_at = kNow;
  EXPECT_EQ(store->MergePatterns({same}, ImportConflictPolicy::Merge, kNow)->conflicts, 1u);
  ASSERT_EQ(store->Patterns().size(), 1u);
  EXPECT_EQ(store->Patterns().front().accept_count, 2u);
  ASSERT_TRUE(store->Save(kNow));
  // The saved database itself refuses a second row of the key.
  const auto saved = DecryptedText(paths_.v2_store);
  EXPECT_FALSE(ExecSucceeds(saved,
                            "INSERT INTO typo_patterns (observed_pattern, intended_pattern, "
                            "typo_type, keyboard_layout, created_at, updated_at) VALUES "
                            "('j_insert_after_u', 'j_delete_after_u', 'extra_char', '', 1, 1)"));
  EXPECT_TRUE(ExecSucceeds(saved,
                           "INSERT INTO typo_patterns (observed_pattern, intended_pattern, "
                           "typo_type, keyboard_layout, created_at, updated_at) VALUES "
                           "('j_insert_after_u', 'j_delete_after_u', 'extra_char', 'jis', 1, 1)"));
}

TEST_F(TypoLearningStoreTest, SavingKeepsTheMostRecentlyUpdatedPatternsAndTheirEvents) {
  auto store = MakeStore(TypoLearningLimits{180 * kDay, 10000, 2});
  store->Load(kNow);
  store->RecordPatternEvent(
      PatternEvent("z_sub_x", "x_sub_z", TypoPatternOutcome::Accept, kNow - 30));
  store->RecordPatternEvent(
      PatternEvent("j_insert_after_u", "j_delete_after_u", TypoPatternOutcome::Accept, kNow - 20));
  store->RecordPatternEvent(
      PatternEvent("ts_swap", "st_swap", TypoPatternOutcome::Accept, kNow - 10));
  ASSERT_TRUE(store->Save(kNow));
  const auto patterns = store->Patterns();
  ASSERT_EQ(patterns.size(), 2u);
  for (const auto& pattern : patterns) EXPECT_NE(pattern.observed_pattern, "z_sub_x");
  EXPECT_EQ(store->EventCount(), 2u);
}

TEST_F(TypoLearningStoreTest, AnUnwritableRebuildDoesNotCopyTheSameCorruptFileAgain) {
  WriteEncrypted(paths_.v2_store, "garbage");
  crypto_.fail_encrypt = true;
  for (const uint64_t now : {kNow, kNow + 1, kNow + 2}) {
    auto store = MakeStore();
    const auto result = store->Load(now);
    EXPECT_EQ(result.mode, TypoLearningMode::V2);
    EXPECT_TRUE(result.rebuilt_from_corruption);
    EXPECT_TRUE(result.rebuilt_store_unsaved);
  }
  EXPECT_EQ(CountCorruptCopies(dir_), 1u);
}

TEST_F(TypoLearningStoreTest, AV1StoreAlreadyAppendedToTheBackupIsNotAppendedAgain) {
  MakeMigratedV2();
  const auto v1_text = DecryptedText(paths_.v1_backup);
  // As after a load that appended the v1 store but could not remove it.
  const std::string backup = std::string(kHeader) + "きょうわ\tきょうは\t1 1\n" + v1_text;
  WriteEncrypted(paths_.v1_backup, backup);
  WriteEncrypted(v1_, v1_text);
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::V2);
  EXPECT_FALSE(result.v1_store_pending);
  EXPECT_EQ(DecryptedText(paths_.v1_backup), backup);
  EXPECT_FALSE(Exists(EncryptedPathFor(v1_)));
}

TEST_F(TypoLearningStoreTest, AV1StoreMatchingOnlyTheTailOfABackupRowIsStillAppended) {
  MakeMigratedV2();
  // A v1 store without the header whose text is the tail of a longer backup row.
  const std::string v1_text = "こんちには\tこんにちは\t3 1700000000\n";
  const std::string backup = std::string(kHeader) + "おこんちには\tこんにちは\t3 1700000000\n";
  WriteEncrypted(paths_.v1_backup, backup);
  WriteEncrypted(v1_, v1_text);
  auto store = MakeStore();
  const auto result = store->Load(kNow);
  EXPECT_EQ(result.mode, TypoLearningMode::V2);
  EXPECT_FALSE(result.v1_store_pending);
  EXPECT_EQ(DecryptedText(paths_.v1_backup), backup + v1_text);
  EXPECT_FALSE(Exists(EncryptedPathFor(v1_)));
}
