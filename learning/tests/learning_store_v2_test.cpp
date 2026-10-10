#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "TestByteCrypto.h"
#include "azookey/learning/ContextHash.h"
#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/LearningDecay.h"
#include "azookey/learning/LearningStore.h"

namespace {

namespace learning = azookey::learning;
using learning::LearningEventType;
using learning::LearningObservation;
using learning::LearningStore;

constexpr uint64_t kNow = 2'000'000'000;
constexpr uint64_t kDay = 24 * 60 * 60;

class ScopedLearningDirectory {
 public:
  explicit ScopedLearningDirectory(const char* name)
      : root_(std::filesystem::temp_directory_path() / name) {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }
  ~ScopedLearningDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }
  ScopedLearningDirectory(const ScopedLearningDirectory&) = delete;
  ScopedLearningDirectory& operator=(const ScopedLearningDirectory&) = delete;

  std::filesystem::path LegacyPath() const { return root_ / "learning.tsv"; }

 private:
  std::filesystem::path root_;
};

std::string ReadBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
}

std::string SavedV2Text(const std::filesystem::path& legacy_path) {
  std::string text;
  EXPECT_EQ(learning::ReadProtectedText(learning::LearningStoreV2PathFor(legacy_path),
                                        learning::test::Crypto(), text),
            learning::ProtectedFileSource::Encrypted);
  return text;
}

LearningObservation Event(LearningEventType event, const char* surface = "交渉",
                          const char* app = "") {
  return LearningObservation{"こうしょう", surface, app, event, ""};
}

}  // namespace

TEST(LearningStoreV2Test, V2PathSitsBesideTheLegacyFile) {
  EXPECT_EQ(learning::LearningStoreV2PathFor(std::filesystem::path("data") / "learning.tsv"),
            std::filesystem::path("data") / "learning.v2.tsv");
}

#ifdef _WIN32
// No ANSI code page can represent U+1F600, so any narrow conversion of the
// path would throw whatever the machine's code page is.
TEST(LearningStoreV2Test, PathsOutsideEveryAnsiCodePageRoundTrip) {
  const auto root = std::filesystem::temp_directory_path() / L"azookey_v2_\U0001F600";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto path = root / L"\U0001F600.tsv";
  EXPECT_EQ(learning::LearningStoreV2PathFor(path).filename(), L"\U0001F600.v2.tsv");

  LearningStore store(path, &learning::test::Crypto());
  store.Observe("にほんご", "日本語", 2.0, kNow);
  ASSERT_TRUE(store.Save());
  LearningStore loaded(path, &learning::test::Crypto());
  ASSERT_TRUE(loaded.Load());
  EXPECT_DOUBLE_EQ(loaded.Score("にほんご", "日本語", kNow), 2.0);
  std::filesystem::remove_all(root);
}
#endif

TEST(LearningStoreV2Test, MigratesM7FileKeepingWeightsAndLeavesLegacyBytesUntouched) {
  ScopedLearningDirectory directory("azookey_learning_v2_migration");
  const auto path = directory.LegacyPath();
  ASSERT_TRUE(learning::WriteProtectedText(path,
                                           "# azookey-learning-tsv escaped=1\n"
                                           "にほん\t日本\t2.4 1999999000\n"
                                           "a\\tb\tA\t0.8 1999999000\n",
                                           learning::test::Crypto()));
  const auto legacy_bytes = ReadBytes(learning::EncryptedPathFor(path));

  LearningStore store(path, &learning::test::Crypto());
  ASSERT_TRUE(store.Load());
  // Migrated rows wait for the next flush to reach the v2 file.
  EXPECT_TRUE(store.dirty());
  EXPECT_DOUBLE_EQ(store.Score("にほん", "日本", 1999999000), 2.4);
  EXPECT_DOUBLE_EQ(store.Score("a\tb", "A", 1999999000), 0.8);
  const auto* rows = store.Rows("にほん", "日本");
  ASSERT_NE(rows, nullptr);
  EXPECT_EQ(rows->at("").commit_count, 3u);
  EXPECT_EQ(rows->at("").last_event, LearningEventType::None);

  ASSERT_TRUE(store.Save());
  EXPECT_EQ(ReadBytes(learning::EncryptedPathFor(path)), legacy_bytes);

  LearningStore reloaded(path, &learning::test::Crypto());
  ASSERT_TRUE(reloaded.Load());
  EXPECT_FALSE(reloaded.dirty());
  EXPECT_DOUBLE_EQ(reloaded.Score("にほん", "日本", 1999999000), 2.4);
  EXPECT_DOUBLE_EQ(reloaded.Score("a\tb", "A", 1999999000), 0.8);
}

TEST(LearningStoreV2Test, UnreadableV2FileBlocksSaveAndKeepsBothFiles) {
  ScopedLearningDirectory directory("azookey_learning_v2_unreadable");
  const auto path = directory.LegacyPath();
  ASSERT_TRUE(
      learning::WriteProtectedText(path, "にほん\t日本\t2 1999999000\n", learning::test::Crypto()));
  const auto v2_encrypted = learning::EncryptedPathFor(learning::LearningStoreV2PathFor(path));
  WriteBytes(v2_encrypted, "not a protected blob");
  const auto legacy_bytes = ReadBytes(learning::EncryptedPathFor(path));

  LearningStore store(path, &learning::test::Crypto());
  EXPECT_FALSE(store.Load());
  EXPECT_EQ(store.size(), 0u);
  store.Observe("あ", "亜", 1.0, kNow);
  EXPECT_FALSE(store.Save());
  EXPECT_EQ(ReadBytes(v2_encrypted), "not a protected blob");
  EXPECT_EQ(ReadBytes(learning::EncryptedPathFor(path)), legacy_bytes);
}

TEST(LearningStoreV2Test, RoundTripsEveryColumnPerApp) {
  ScopedLearningDirectory directory("azookey_learning_v2_round_trip");
  const auto path = directory.LegacyPath();
  LearningStore store(path, &learning::test::Crypto());
  store.ObserveEvent({"にほんご", "日本語", "code.exe", LearningEventType::Commit, "0xabcd1234"},
                     0.8, kNow);
  store.ObserveEvent({"にほんご", "日本語", "", LearningEventType::CorrectionReject, ""}, 0.8,
                     kNow + 1);
  ASSERT_TRUE(store.Save());

  EXPECT_EQ(SavedV2Text(path),
            "# azookey-learning-tsv escaped=1 version=2\n"
            "にほんご\t日本語\t0\t2000000001\t0\t0\t1\t\tcorrection_reject\t\n"
            "にほんご\t日本語\t0.8\t2000000000\t1\t0\t0\tcode.exe\tcommit\t0xabcd1234\n");

  LearningStore loaded(path, &learning::test::Crypto());
  ASSERT_TRUE(loaded.Load());
  EXPECT_EQ(loaded.size(), 2u);
  const auto* rows = loaded.Rows("にほんご", "日本語");
  ASSERT_NE(rows, nullptr);
  const auto& app_row = rows->at("code.exe");
  EXPECT_DOUBLE_EQ(app_row.weight, 0.8);
  EXPECT_EQ(app_row.commit_count, 1u);
  EXPECT_EQ(app_row.last_event, LearningEventType::Commit);
  EXPECT_EQ(app_row.context_hash, "0xabcd1234");
  EXPECT_EQ(rows->at("").reject_count, 1u);
  EXPECT_DOUBLE_EQ(loaded.Score("にほんご", "日本語", kNow), 0.8);
}

TEST(LearningStoreV2Test, SkipsMalformedV2Rows) {
  LearningStore store("unused.tsv", &learning::test::Crypto());
  store.LoadText(
      "# azookey-learning-tsv escaped=1 version=2\n"
      "ok\tOK\t1\t100\t1\t0\t0\t\tcommit\t0x00000000\n"
      "short\tS\t1\t100\t1\t0\t0\t\tcommit\n"
      "event\tE\t1\t100\t1\t0\t0\t\tunknown\t\n"
      "hash\tH\t1\t100\t1\t0\t0\t\tcommit\t0xABCD1234\n"
      "negative\tN\t-1\t100\t1\t0\t0\t\tcommit\t\n"
      "\tempty-reading\t1\t100\t1\t0\t0\t\tcommit\t\n");
  EXPECT_EQ(store.size(), 1u);
  EXPECT_GT(store.Score("ok", "OK", 100), 0.0);
}

TEST(LearningStoreV2Test, EachEventMovesWeightAndCountsAsSpecified) {
  constexpr double kAlpha = 0.8;
  LearningStore store("unused.tsv", &learning::test::Crypto());
  store.ObserveEvent(Event(LearningEventType::Commit), kAlpha, kNow);
  store.ObserveEvent(Event(LearningEventType::CorrectionAccept), kAlpha, kNow);
  store.ObserveEvent(Event(LearningEventType::TypoAccept), kAlpha, kNow);
  auto record = store.Rows("こうしょう", "交渉")->at("");
  EXPECT_DOUBLE_EQ(record.weight, kAlpha + kAlpha + learning::kTypoAcceptWeight);
  EXPECT_EQ(record.commit_count, 3u);
  EXPECT_EQ(record.accept_count, 2u);

  store.ObserveEvent(Event(LearningEventType::CorrectionReject), kAlpha, kNow);
  store.ObserveEvent(Event(LearningEventType::TypoReject), kAlpha, kNow);
  record = store.Rows("こうしょう", "交渉")->at("");
  EXPECT_NEAR(record.weight, kAlpha + learning::kTypoAcceptWeight - learning::kTypoRejectWeight,
              1e-12);
  EXPECT_EQ(record.reject_count, 2u);
  EXPECT_EQ(record.last_event, LearningEventType::TypoReject);

  // Rejections floor at zero instead of going negative.
  store.ObserveEvent(Event(LearningEventType::CorrectionReject, "校章"), kAlpha, kNow);
  EXPECT_DOUBLE_EQ(store.Rows("こうしょう", "校章")->at("").weight, 0.0);

  // An app commit is its own row; Score sums the rows.
  store.ObserveEvent(Event(LearningEventType::Commit, "交渉", "outlook.exe"), kAlpha, kNow);
  EXPECT_EQ(store.Rows("こうしょう", "交渉")->size(), 2u);
  EXPECT_NEAR(store.Score("こうしょう", "交渉", kNow),
              kAlpha + learning::kTypoAcceptWeight - learning::kTypoRejectWeight + kAlpha, 1e-12);

  // None is not an event and changes nothing.
  const auto size = store.size();
  store.ObserveEvent(Event(LearningEventType::None, "なし"), kAlpha, kNow);
  EXPECT_EQ(store.size(), size);
}

TEST(LearningStoreV2Test, ObserveCorrectionRecordsAcceptAndReject) {
  LearningStore store("unused.tsv", &learning::test::Crypto());
  store.ObserveCorrection("にほん", "日本", "二本", 0.5, kNow);
  EXPECT_EQ(store.Rows("にほん", "二本")->at("").accept_count, 1u);
  EXPECT_EQ(store.Rows("にほん", "日本")->at("").reject_count, 1u);
}

TEST(LearningStoreV2Test, ForgetZeroesEveryAppRowAndSaveDropsThePair) {
  ScopedLearningDirectory directory("azookey_learning_v2_forget");
  const auto path = directory.LegacyPath();
  LearningStore store(path, &learning::test::Crypto());
  store.ObserveEvent(Event(LearningEventType::Commit), 0.8, kNow);
  store.ObserveEvent(Event(LearningEventType::Commit, "交渉", "outlook.exe"), 0.8, kNow);
  store.Observe("こうしょう", "校章", 0.8, kNow);
  ASSERT_TRUE(store.Save());

  EXPECT_FALSE(store.Forget("こうしょう", "未学習"));
  EXPECT_TRUE(store.Forget("こうしょう", "交渉"));
  EXPECT_TRUE(store.dirty());
  EXPECT_DOUBLE_EQ(store.Score("こうしょう", "交渉", kNow), 0.0);
  EXPECT_EQ(store.Aggregates().size(), 1u);
  ASSERT_TRUE(store.Save());
  EXPECT_EQ(SavedV2Text(path).find("交渉"), std::string::npos);

  // A later commit starts from scratch, as if it were the first.
  store.Observe("こうしょう", "交渉", 0.8, kNow + kDay);
  const auto& record = store.Rows("こうしょう", "交渉")->at("");
  EXPECT_DOUBLE_EQ(record.weight, 0.8);
  EXPECT_EQ(record.commit_count, 1u);
}

namespace {
std::string LegacyText(const std::filesystem::path& legacy_path) {
  std::string text;
  EXPECT_EQ(learning::ReadProtectedText(legacy_path, learning::test::Crypto(), text),
            learning::ProtectedFileSource::Encrypted);
  return text;
}
}  // namespace

TEST(LearningStoreV2Test, RemoveFromLegacyFileDropsOnlyThePairFromAnEscapedFile) {
  ScopedLearningDirectory directory("azookey_learning_v2_legacy_forget_escaped");
  const auto path = directory.LegacyPath();
  ASSERT_TRUE(learning::WriteProtectedText(path,
                                           "# azookey-learning-tsv escaped=1\n"
                                           "こうしょう\t交渉\t2.4 1999999000\n"
                                           "a\\tb\tA\t0.8 1999999000\n"
                                           "broken row\n"
                                           "\n"
                                           "こうしょう\t校章\t0.8   1999999001\r\n"
                                           "a\\tb\tA\t1.6 1999999002\n"
                                           "a\\tb\tB\t0.8 1999999000",
                                           learning::test::Crypto()));
  const auto v2_encrypted = learning::EncryptedPathFor(learning::LearningStoreV2PathFor(path));

  LearningStore store(path, &learning::test::Crypto());
  // The escaped reading "a\tb" holds a real tab; both of its rows go.
  ASSERT_TRUE(store.RemoveFromLegacyFile("a\tb", "A"));
  EXPECT_EQ(LegacyText(path),
            "# azookey-learning-tsv escaped=1\n"
            "こうしょう\t交渉\t2.4 1999999000\n"
            "broken row\n"
            "\n"
            "こうしょう\t校章\t0.8   1999999001\r\n"
            "a\\tb\tB\t0.8 1999999000");
  ASSERT_TRUE(store.RemoveFromLegacyFile("こうしょう", "交渉"));
  EXPECT_EQ(LegacyText(path),
            "# azookey-learning-tsv escaped=1\n"
            "broken row\n"
            "\n"
            "こうしょう\t校章\t0.8   1999999001\r\n"
            "a\\tb\tB\t0.8 1999999000");
  EXPECT_FALSE(std::filesystem::exists(v2_encrypted));
}

TEST(LearningStoreV2Test, RemoveFromLegacyFileComparesRawFieldsWithoutTheHeader) {
  ScopedLearningDirectory directory("azookey_learning_v2_legacy_forget_raw");
  const auto path = directory.LegacyPath();
  // Without the header the fields are not escaped: "a\\tb" is a backslash and a t.
  ASSERT_TRUE(learning::WriteProtectedText(path,
                                           "a\\tb\tA\t0.8 1999999000\n"
                                           "にほん\t日本\t2 1999999000\n",
                                           learning::test::Crypto()));
  const auto before = ReadBytes(learning::EncryptedPathFor(path));

  LearningStore store(path, &learning::test::Crypto());
  ASSERT_TRUE(store.RemoveFromLegacyFile("a\tb", "A"));
  // No row matched, so nothing was written (a rewrite would re-encrypt).
  EXPECT_EQ(ReadBytes(learning::EncryptedPathFor(path)), before);
  ASSERT_TRUE(store.RemoveFromLegacyFile("a\\tb", "A"));
  EXPECT_EQ(LegacyText(path), "にほん\t日本\t2 1999999000\n");
}

// learning-data-management-spec section 4.6: the reset drops every M7 row and
// keeps the header and the lines that are not rows.
TEST(LearningStoreV2Test, ClearLegacyFileDropsEveryRow) {
  ScopedLearningDirectory directory("azookey_learning_v2_legacy_clear");
  const auto path = directory.LegacyPath();
  ASSERT_TRUE(learning::WriteProtectedText(path,
                                           "# azookey-learning-tsv escaped=1\n"
                                           "こうしょう\t交渉\t2.4 1999999000\n"
                                           "broken row\n"
                                           "a\\tb\tA\t0.8 1999999000\n",
                                           learning::test::Crypto()));
  LearningStore store(path, &learning::test::Crypto());
  ASSERT_TRUE(store.ClearLegacyFile());
  EXPECT_EQ(LegacyText(path),
            "# azookey-learning-tsv escaped=1\n"
            "broken row\n");
  EXPECT_FALSE(
      std::filesystem::exists(learning::EncryptedPathFor(learning::LearningStoreV2PathFor(path))));

  ScopedLearningDirectory missing("azookey_learning_v2_legacy_clear_missing");
  LearningStore empty(missing.LegacyPath(), &learning::test::Crypto());
  EXPECT_TRUE(empty.ClearLegacyFile());
  EXPECT_FALSE(std::filesystem::exists(learning::EncryptedPathFor(missing.LegacyPath())));
}

TEST(LearningStoreV2Test, RemoveFromLegacyFileIsANoOpWithoutTheFile) {
  ScopedLearningDirectory directory("azookey_learning_v2_legacy_forget_missing");
  const auto path = directory.LegacyPath();
  LearningStore store(path, &learning::test::Crypto());
  EXPECT_TRUE(store.RemoveFromLegacyFile("こうしょう", "交渉"));
  EXPECT_FALSE(std::filesystem::exists(learning::EncryptedPathFor(path)));
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(LearningStoreV2Test, RemoveFromLegacyFileLeavesAnUnreadableFileUntouched) {
  ScopedLearningDirectory directory("azookey_learning_v2_legacy_forget_unreadable");
  const auto path = directory.LegacyPath();
  WriteBytes(learning::EncryptedPathFor(path), "not a protected blob");

  LearningStore store(path, &learning::test::Crypto());
  EXPECT_FALSE(store.RemoveFromLegacyFile("こうしょう", "交渉"));
  EXPECT_EQ(ReadBytes(learning::EncryptedPathFor(path)), "not a protected blob");
}

TEST(LearningStoreV2Test, RemoveFromLegacyFileRefusesUnmigratedPlaintext) {
  ScopedLearningDirectory directory("azookey_learning_v2_legacy_forget_plaintext");
  const auto path = directory.LegacyPath();
  const std::string plaintext = "こうしょう\t交渉\t2 1999999000\nにほん\t日本\t2 1999999000\n";
  WriteBytes(path, plaintext);

  LearningStore store(path, &learning::test::Crypto());
  // WriteProtectedText refuses to replace plaintext that was never migrated to
  // .enc (no .bak kept yet), so the forget fails and the file stays as it is.
  // Load migrates the file first; after that the removal succeeds.
  EXPECT_FALSE(store.RemoveFromLegacyFile("こうしょう", "交渉"));
  EXPECT_EQ(ReadBytes(path), plaintext);
  EXPECT_FALSE(std::filesystem::exists(learning::EncryptedPathFor(path)));

  ASSERT_TRUE(store.Load());
  ASSERT_TRUE(store.RemoveFromLegacyFile("こうしょう", "交渉"));
  EXPECT_EQ(LegacyText(path), "にほん\t日本\t2 1999999000\n");
}

TEST(LearningStoreV2Test, MergePoliciesDecideExistingRowsOnly) {
  LearningStore imported("unused.tsv", &learning::test::Crypto());
  imported.ObserveEvent(Event(LearningEventType::Commit), 0.5, kNow + 10);
  imported.Observe("しんき", "新規", 0.5, kNow);
  imported.Observe("わすれ", "忘れ", 0.5, kNow);
  imported.Forget("わすれ", "忘れ");

  const auto local = [] {
    auto store = std::make_unique<LearningStore>("unused.tsv", &learning::test::Crypto());
    store->ObserveEvent(Event(LearningEventType::Commit), 1.0, kNow);
    return store;
  };

  auto merged = local();
  auto counts = merged->Merge(imported, learning::ImportConflictPolicy::Merge);
  EXPECT_EQ(counts.imported, 2u);
  EXPECT_EQ(counts.conflicts, 1u);
  EXPECT_EQ(counts.skipped, 1u);
  const auto& sum = merged->Rows("こうしょう", "交渉")->at("");
  EXPECT_DOUBLE_EQ(sum.weight, 1.5);
  EXPECT_EQ(sum.commit_count, 2u);
  EXPECT_EQ(sum.last_updated_epoch_sec, kNow + 10);
  EXPECT_NE(merged->Rows("しんき", "新規"), nullptr);
  EXPECT_EQ(merged->Rows("わすれ", "忘れ"), nullptr);

  auto overwritten = local();
  overwritten->Merge(imported, learning::ImportConflictPolicy::Overwrite);
  EXPECT_DOUBLE_EQ(overwritten->Rows("こうしょう", "交渉")->at("").weight, 0.5);

  auto kept = local();
  counts = kept->Merge(imported, learning::ImportConflictPolicy::KeepBoth);
  EXPECT_EQ(counts.imported, 1u);
  EXPECT_EQ(counts.skipped, 2u);
  EXPECT_DOUBLE_EQ(kept->Rows("こうしょう", "交渉")->at("").weight, 1.0);
}

TEST(LearningStoreV2Test, MergeTreatsAForgottenLocalRowAsAbsent) {
  LearningStore imported("unused.tsv", &learning::test::Crypto());
  imported.Observe("こうしょう", "交渉", 0.5, kNow);
  LearningStore local("unused.tsv", &learning::test::Crypto());
  local.Observe("こうしょう", "交渉", 1.0, kNow);
  local.Forget("こうしょう", "交渉");

  const auto counts = local.Merge(imported, learning::ImportConflictPolicy::KeepBoth);
  EXPECT_EQ(counts.imported, 1u);
  EXPECT_EQ(counts.conflicts, 0u);
  EXPECT_DOUBLE_EQ(local.Score("こうしょう", "交渉", kNow), 0.5);
}

TEST(LearningStoreV2Test, LoadTextReportsMalformedRows) {
  LearningStore store("unused.tsv", &learning::test::Crypto());
  EXPECT_TRUE(store.LoadText("a\tA\t1 1\n"));
  EXPECT_FALSE(store.LoadText("a\tA\t1 1\nbroken\n"));
  EXPECT_EQ(store.size(), 1u);
}

TEST(LearningStoreV2Test, ForgottenPairsNeverAppearInPrefixLookup) {
  LearningStore store("unused.tsv", &learning::test::Crypto());
  store.Observe("こうしょう", "交渉", 0.8, kNow);
  store.Observe("こうしょう", "校章", 0.8, kNow);
  store.Forget("こうしょう", "交渉");
  const auto result = store.LookupPrefix("こう", 10, /*min_score=*/0.0, kNow);
  ASSERT_EQ(result.matches.size(), 1u);
  EXPECT_EQ(result.matches.front().surface, "校章");
}

TEST(LearningStoreV2Test, PruneJudgesThePairAndKeepsNetRejections) {
  LearningStore store("unused.tsv", &learning::test::Crypto());
  // Two app rows below the threshold each, above it together.
  store.ObserveEvent(Event(LearningEventType::Commit, "交渉", "a.exe"), 0.03, kNow);
  store.ObserveEvent(Event(LearningEventType::Commit, "交渉", "b.exe"), 0.03, kNow);
  // Committed three times, then rejected three times: weight 0.
  for (int i = 0; i < 3; ++i)
    store.ObserveEvent(Event(LearningEventType::Commit, "校章"), 0.8, kNow);
  for (int i = 0; i < 3; ++i) {
    store.ObserveEvent(Event(LearningEventType::CorrectionReject, "校章"), 0.8, kNow);
  }
  store.Observe("こうしょう", "高尚", 0.01, kNow);

  store.Prune(/*max_records=*/0, /*min_weight=*/0.05, kNow);
  EXPECT_EQ(store.Rows("こうしょう", "交渉")->size(), 2u);
  // The rejections, and so the section 6.2 penalty, survive the GC.
  ASSERT_NE(store.Rows("こうしょう", "校章"), nullptr);
  EXPECT_EQ(store.Rows("こうしょう", "校章")->at("").reject_count, 3u);
  EXPECT_EQ(store.Rows("こうしょう", "高尚"), nullptr);
}

TEST(LearningStoreV2Test, ObserveEventStoresTheNormalizedAppName) {
  LearningStore store("unused.tsv", &learning::test::Crypto());
  store.ObserveEvent(Event(LearningEventType::Commit, "交渉", "C:\\Office\\OUTLOOK.EXE"), 0.8,
                     kNow);
  const auto* rows = store.Rows("こうしょう", "交渉");
  ASSERT_NE(rows, nullptr);
  EXPECT_EQ(rows->count("outlook.exe"), 1u);
}

TEST(LearningStoreV2Test, FirstV2SaveRefusesWhileTheLegacyFileHasAnOrphanBackup) {
  ScopedLearningDirectory directory("azookey_learning_v2_orphan_backup");
  const auto path = directory.LegacyPath();
  auto backup = path;
  backup += ".bak";
  WriteBytes(backup, "にほん\t日本\t2 100\n");

  LearningStore store(path, &learning::test::Crypto());
  store.Observe("あ", "亜", 1.0, kNow);
  // Writing v2 now would shadow the only copy of the M7 data for good.
  EXPECT_FALSE(store.Save());
  EXPECT_FALSE(
      std::filesystem::exists(learning::EncryptedPathFor(learning::LearningStoreV2PathFor(path))));
  EXPECT_EQ(ReadBytes(backup), "にほん\t日本\t2 100\n");
}

TEST(LearningStoreV2Test, ExistingV2FileIsSavedWhateverTheLegacyFileHolds) {
  ScopedLearningDirectory directory("azookey_learning_v2_ignores_legacy");
  const auto path = directory.LegacyPath();
  LearningStore first(path, &learning::test::Crypto());
  first.Observe("あ", "亜", 1.0, kNow);
  ASSERT_TRUE(first.Save());
  auto backup = path;
  backup += ".bak";
  WriteBytes(backup, "stale\tbackup\t1 1\n");

  LearningStore store(path, &learning::test::Crypto());
  ASSERT_TRUE(store.Load());
  store.Observe("い", "伊", 1.0, kNow);
  EXPECT_TRUE(store.Save());
  EXPECT_EQ(ReadBytes(backup), "stale\tbackup\t1 1\n");
}

TEST(LearningStoreV2Test, MalformedV2RowsAreKeptAsideBeforeTheNextSave) {
  ScopedLearningDirectory directory("azookey_learning_v2_keep_aside");
  const auto path = directory.LegacyPath();
  const auto v2_path = learning::LearningStoreV2PathFor(path);
  ASSERT_TRUE(learning::WriteProtectedText(v2_path,
                                           "# azookey-learning-tsv escaped=1 version=2\n"
                                           "ok\tOK\t1\t100\t1\t0\t0\t\tcommit\t\n"
                                           "broken row\n",
                                           learning::test::Crypto()));
  const auto original = ReadBytes(learning::EncryptedPathFor(v2_path));

  LearningStore store(path, &learning::test::Crypto());
  ASSERT_TRUE(store.Load());
  EXPECT_EQ(store.size(), 1u);
  store.Observe("新", "規", 1.0, kNow);
  ASSERT_TRUE(store.Save());

  std::vector<std::filesystem::path> copies;
  for (const auto& entry : std::filesystem::directory_iterator(v2_path.parent_path())) {
    if (entry.path().filename().string().find(".corrupt-") != std::string::npos) {
      copies.push_back(entry.path());
    }
  }
  ASSERT_EQ(copies.size(), 1u);
  EXPECT_EQ(ReadBytes(copies.front()), original);

  LearningStore reloaded(path, &learning::test::Crypto());
  ASSERT_TRUE(reloaded.Load());
  EXPECT_GT(reloaded.Score("ok", "OK", 100), 0.0);
  EXPECT_GT(reloaded.Score("新", "規", kNow), 0.0);
  // A clean file needs no further copy.
  ASSERT_TRUE(reloaded.Save());
  size_t after = 0;
  for (const auto& entry : std::filesystem::directory_iterator(v2_path.parent_path())) {
    after += entry.path().filename().string().find(".corrupt-") != std::string::npos ? 1 : 0;
  }
  EXPECT_EQ(after, 1u);
}

TEST(LearningStoreV2Test, UnreadableLegacyFileBlocksSaveDuringMigration) {
  ScopedLearningDirectory directory("azookey_learning_v2_unreadable_legacy");
  const auto path = directory.LegacyPath();
  WriteBytes(learning::EncryptedPathFor(path), "not a protected blob");

  LearningStore store(path, &learning::test::Crypto());
  EXPECT_FALSE(store.Load());
  EXPECT_TRUE(store.save_blocked());
  store.Observe("あ", "亜", 1.0, kNow);
  EXPECT_FALSE(store.Save());
  EXPECT_FALSE(
      std::filesystem::exists(learning::EncryptedPathFor(learning::LearningStoreV2PathFor(path))));
}

TEST(LearningStoreV2Test, ImportedValuesAreBoundedAndMergesSaturate) {
  LearningStore imported("unused.tsv", &learning::test::Crypto());
  ASSERT_TRUE(
      imported.LoadText("# azookey-learning-tsv escaped=1 version=2\n"
                        "a\tA\t1e300\t1\t18446744073709551615\t0\t0\t\tcommit\t\n"));
  const auto& bounded = imported.Rows("a", "A")->at("");
  EXPECT_DOUBLE_EQ(bounded.weight, learning::kMaxLearningWeight);
  EXPECT_EQ(bounded.commit_count, learning::kMaxLearningCount);

  LearningStore local("unused.tsv", &learning::test::Crypto());
  ASSERT_TRUE(
      local.LoadText("# azookey-learning-tsv escaped=1 version=2\n"
                     "a\tA\t1e300\t1\t18446744073709551615\t0\t0\t\tcommit\t\n"));
  local.Merge(imported, learning::ImportConflictPolicy::Merge);
  const auto& merged = local.Rows("a", "A")->at("");
  EXPECT_DOUBLE_EQ(merged.weight, learning::kMaxLearningWeight);
  EXPECT_EQ(merged.commit_count, learning::kMaxLearningCount);
  EXPECT_TRUE(std::isfinite(local.Score("a", "A", 1)));
}

TEST(LearningStoreV2Test, WeightsSurviveRepeatedSaveAndLoadExactly) {
  ScopedLearningDirectory directory("azookey_learning_v2_round_trip_weight");
  const auto path = directory.LegacyPath();
  const double weight = 0.1 + 0.2;
  {
    LearningStore store(path, &learning::test::Crypto());
    store.Observe("あ", "亜", weight, kNow);
    ASSERT_TRUE(store.Save());
  }
  for (int i = 0; i < 3; ++i) {
    LearningStore store(path, &learning::test::Crypto());
    ASSERT_TRUE(store.Load());
    store.Observe("い", "伊", 1.0, kNow);
    ASSERT_TRUE(store.Save());
  }
  LearningStore loaded(path, &learning::test::Crypto());
  ASSERT_TRUE(loaded.Load());
  EXPECT_EQ(loaded.Rows("あ", "亜")->at("").weight, weight);
}

TEST(LearningStoreV2Test, CountValidLearningRowsRejectsAnyMalformedRow) {
  EXPECT_EQ(learning::CountValidLearningRows("# azookey-learning-tsv escaped=1 version=2\n"
                                             "a\tA\t1\t1\t1\t0\t0\t\tcommit\t\n"),
            1u);
  EXPECT_EQ(learning::CountValidLearningRows("a\tA\t1 1\n\nb\tB\t2 2\n"), 2u);
  EXPECT_FALSE(learning::CountValidLearningRows("a\tA\t1 1\nbroken\n").has_value());
}

TEST(LearningStoreV2Test, ConcurrentSavesLeaveOneCompleteFile) {
  ScopedLearningDirectory directory("azookey_learning_v2_concurrent");
  const auto path = directory.LegacyPath();
  LearningStore first(path, &learning::test::Crypto());
  LearningStore second(path, &learning::test::Crypto());
  for (int i = 0; i < 50; ++i) {
    first.Observe("first" + std::to_string(i), "F", 1.0, kNow);
    second.Observe("second" + std::to_string(i), "S", 1.0, kNow);
  }
  std::thread a([&] { EXPECT_TRUE(first.Save(std::chrono::milliseconds(2000))); });
  std::thread b([&] { EXPECT_TRUE(second.Save(std::chrono::milliseconds(2000))); });
  a.join();
  b.join();

  LearningStore loaded(path, &learning::test::Crypto());
  ASSERT_TRUE(loaded.Load());
  // The file lock serializes the writers: one complete snapshot, never a mix.
  EXPECT_EQ(loaded.size(), 50u);
  EXPECT_TRUE(loaded.Score("first0", "F", kNow) > 0.0 || loaded.Score("second0", "S", kNow) > 0.0);
}

TEST(LearningStoreV2Test, NormalizesAppNamesToLowercaseBasename) {
  EXPECT_EQ(learning::NormalizeLearningAppName("C:\\Program Files\\App\\Code.EXE"), "code.exe");
  EXPECT_EQ(learning::NormalizeLearningAppName("/usr/bin/Vim"), "vim");
  EXPECT_EQ(learning::NormalizeLearningAppName(""), "");
}

TEST(ContextHashTest, EmptyContextIsTheZeroHash) {
  EXPECT_EQ(learning::ContextHash(""), "0x00000000");
}

TEST(ContextHashTest, HashesOnlyTheLastEightCodePoints) {
  // SHA-256("はいい天気ですね") starts with 94e5fb33.
  EXPECT_EQ(learning::ContextHash("今日はいい天気ですね"), "0x94e5fb33");
  EXPECT_EQ(learning::ContextHash("明日はいい天気ですね"), "0x94e5fb33");
  EXPECT_NE(learning::ContextHash("いい天気ですね"), "0x94e5fb33");
}

TEST(ContextHashTest, OutputCannotCarryTheOriginalText) {
  const std::string context = "パスワードは秘密です";
  const auto hash = learning::ContextHash(context);
  ASSERT_EQ(hash.size(), 10u);
  EXPECT_EQ(hash.substr(0, 2), "0x");
  EXPECT_EQ(hash.find("秘密"), std::string::npos);
  // Only 32 bits survive, far fewer than the 8 code points they summarize, so
  // the text cannot be recovered from the stored value.
  EXPECT_EQ(hash.find_first_not_of("0123456789abcdef", 2), std::string::npos);
}

TEST(ContextHashTest, InvalidUtf8DoesNotCrash) {
  const std::string invalid = "\xE3\x81";
  EXPECT_EQ(learning::ContextHash(invalid).size(), 10u);
}

#ifdef _WIN32
TEST(ContextHashTest, NormalizesToNfcBeforeHashing) {
  // "が" composed (U+304C) and decomposed (U+304B U+3099).
  EXPECT_EQ(learning::ContextHash("\xE3\x81\x8C"),
            learning::ContextHash("\xE3\x81\x8B\xE3\x82\x99"));
}
#endif

TEST(LearningDecayTest, HalfLifeRecencyHalvesAtTheHalfLife) {
  EXPECT_NEAR(learning::HalfLifeRecency(kNow, kNow + 30 * kDay, 30.0), 0.5, 1e-12);
  EXPECT_DOUBLE_EQ(learning::HalfLifeRecency(kNow + kDay, kNow, 30.0), 1.0);
  EXPECT_DOUBLE_EQ(learning::LegacyRecency(kNow, kNow + kDay), std::exp(-0.15));
}

TEST(LearningDecayTest, CategoryHalfLivesKeepTheSpecifiedOrder) {
  constexpr uint16_t kPersonName = 1U << 1;
  constexpr uint16_t kTechnical = 1U << 8;
  constexpr uint16_t kNeologism = 1U << 9;
  EXPECT_DOUBLE_EQ(learning::HalfLifeDaysForCategoryMask(0), learning::kGeneralHalfLifeDays);
  EXPECT_DOUBLE_EQ(learning::HalfLifeDaysForCategoryMask(1), learning::kGeneralHalfLifeDays);
  EXPECT_DOUBLE_EQ(learning::HalfLifeDaysForCategoryMask(kPersonName),
                   learning::kProperNounHalfLifeDays);
  // Every proper-noun bit retains 90 days, including product_name / software
  // that also map to the Host's Technical tag (not the 120-day technical category).
  for (uint16_t category = 1; category <= 7; ++category) {
    SCOPED_TRACE(category);
    const auto mask = static_cast<uint16_t>(1U << category);
    EXPECT_DOUBLE_EQ(learning::HalfLifeDaysForCategoryMask(mask),
                     learning::kProperNounHalfLifeDays);
    EXPECT_DOUBLE_EQ(learning::HalfLifeDaysForCategoryMask(mask | kNeologism),
                     learning::kProperNounHalfLifeDays);
    EXPECT_DOUBLE_EQ(learning::HalfLifeDaysForCategoryMask(mask | kTechnical),
                     learning::kTechnicalHalfLifeDays);
  }
  EXPECT_DOUBLE_EQ(learning::HalfLifeDaysForCategoryMask(kTechnical),
                   learning::kTechnicalHalfLifeDays);
  EXPECT_DOUBLE_EQ(learning::HalfLifeDaysForCategoryMask(kNeologism),
                   learning::kTypoPatternHalfLifeDays);
  // Several categories: the longest half life wins.
  EXPECT_DOUBLE_EQ(learning::HalfLifeDaysForCategoryMask(kPersonName | kTechnical),
                   learning::kTechnicalHalfLifeDays);
  // Section 5 invariant.
  EXPECT_LT(learning::kTemporaryTopicHalfLifeDays, learning::kGeneralHalfLifeDays);
  EXPECT_LT(learning::kGeneralHalfLifeDays, learning::kTypoPatternHalfLifeDays);
  EXPECT_LT(learning::kTypoPatternHalfLifeDays, learning::kTechnicalHalfLifeDays);
  EXPECT_LT(learning::kProperNounHalfLifeDays, learning::kTechnicalHalfLifeDays);
}
