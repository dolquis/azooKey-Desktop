#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "azookey/core/Candidate.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/Reranker.h"

TEST(LearningStoreTest, SaveLoadAndCorrectionDownweight) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "azookey_learning_test.tsv").string();
  std::remove(path.c_str());

  azookey::learning::LearningStore store(path);
  store.Observe("にほん", "日本", 1.0, 100);
  store.ObserveCorrection("にほん", "日本", "二本", 0.5, 120);
  store.Save();

  azookey::learning::LearningStore loaded(path);
  loaded.Load();
  azookey::learning::Reranker reranker(&loaded);

  std::vector<azookey::core::Candidate> cands = {
      {"日本", "にほん", 0.9, azookey::core::CandidateSource::SystemDictionary, "base"},
      {"二本", "にほん", 0.9, azookey::core::CandidateSource::SystemDictionary, "base"},
  };

  auto ranked = reranker.Apply("にほん", std::move(cands), 120);
  (void)ranked;
  EXPECT_LE(loaded.Score("にほん", "日本", 120), loaded.Score("にほん", "二本", 120));

  std::remove(path.c_str());
}

TEST(LearningStoreTest, SaveCreatesParentAndLeavesNoTempFile) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_learning_atomic_test";
  const auto path = root / "nested" / "learning.tsv";
  std::filesystem::remove_all(root);

  azookey::learning::LearningStore store(path.string());
  store.Observe("とうきょう", "東京", 1.0, 200);
  EXPECT_TRUE(store.Save());
  EXPECT_TRUE(std::filesystem::exists(path));

  size_t temp_files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
    if (entry.path().filename().string().find(".tmp.") != std::string::npos) {
      ++temp_files;
    }
  }
  EXPECT_EQ(temp_files, 0u);

  azookey::learning::LearningStore loaded(path.string());
  EXPECT_TRUE(loaded.Load());
  EXPECT_GT(loaded.Score("とうきょう", "東京", 200), 0.0);

  std::filesystem::remove_all(root);
}

TEST(LearningStoreTest, SaveLoadEscapesTsvSpecialCharactersInSurface) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "azookey_learning_escape_test.tsv").string();
  std::remove(path.c_str());

  azookey::learning::LearningStore store(path);
  store.Observe("にほん", "日本", 1.0, 300);
  store.Observe("にほん", "日\t本", 2.0, 300);
  store.Observe("にほん", "日\n本", 3.0, 300);
  store.Observe("にほん", "C:\\temp", 4.0, 300);
  EXPECT_TRUE(store.Save());

  azookey::learning::LearningStore loaded(path);
  EXPECT_TRUE(loaded.Load());
  EXPECT_EQ(loaded.size(), 4u);
  EXPECT_DOUBLE_EQ(loaded.Score("にほん", "日本", 300), 1.0);
  EXPECT_DOUBLE_EQ(loaded.Score("にほん", "日\t本", 300), 2.0);
  EXPECT_DOUBLE_EQ(loaded.Score("にほん", "日\n本", 300), 3.0);
  EXPECT_DOUBLE_EQ(loaded.Score("にほん", "C:\\temp", 300), 4.0);

  std::remove(path.c_str());
}

TEST(LearningStoreTest, SaveLoadEscapesTsvSpecialCharactersInReading) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "azookey_learning_reading_escape_test.tsv")
          .string();
  std::remove(path.c_str());

  azookey::learning::LearningStore store(path);
  store.Observe("a\tb", "surface", 1.0, 350);
  store.Observe("a\nb", "surface", 2.0, 350);
  store.Observe("C:\\temp", "surface", 3.0, 350);
  EXPECT_TRUE(store.Save());

  azookey::learning::LearningStore loaded(path);
  EXPECT_TRUE(loaded.Load());
  EXPECT_EQ(loaded.size(), 3u);
  EXPECT_DOUBLE_EQ(loaded.Score("a\tb", "surface", 350), 1.0);
  EXPECT_DOUBLE_EQ(loaded.Score("a\nb", "surface", 350), 2.0);
  EXPECT_DOUBLE_EQ(loaded.Score("C:\\temp", "surface", 350), 3.0);
  EXPECT_DOUBLE_EQ(loaded.Score("a", "b\tsurface", 350), 0.0);

  std::remove(path.c_str());
}

TEST(LearningStoreTest, LegacyTsvKeepsBackslashSequencesLiteral) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "azookey_learning_legacy_escape_test.tsv").string();
  std::remove(path.c_str());

  {
    std::ofstream ofs(path);
    ofs << "legacy\tC:\\temp\t2.0 500\n";
    ofs << "legacy\tC:\\new\t3.0 500\n";
  }

  azookey::learning::LearningStore loaded(path);
  EXPECT_TRUE(loaded.Load());
  EXPECT_EQ(loaded.size(), 2u);
  EXPECT_DOUBLE_EQ(loaded.Score("legacy", "C:\\temp", 500), 2.0);
  EXPECT_DOUBLE_EQ(loaded.Score("legacy", "C:\\new", 500), 3.0);
  EXPECT_DOUBLE_EQ(loaded.Score("legacy", std::string("C:") + '\t' + "emp", 500), 0.0);
  EXPECT_DOUBLE_EQ(loaded.Score("legacy", std::string("C:") + '\n' + "ew", 500), 0.0);

  EXPECT_TRUE(loaded.Save());

  azookey::learning::LearningStore migrated(path);
  EXPECT_TRUE(migrated.Load());
  EXPECT_EQ(migrated.size(), 2u);
  EXPECT_DOUBLE_EQ(migrated.Score("legacy", "C:\\temp", 500), 2.0);
  EXPECT_DOUBLE_EQ(migrated.Score("legacy", "C:\\new", 500), 3.0);

  std::remove(path.c_str());
}

TEST(LearningStoreTest, LoadSkipsMalformedRowsAndKeepsValidRows) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "azookey_learning_malformed_test.tsv").string();
  std::remove(path.c_str());

  {
    std::ofstream ofs(path);
    ofs << "valid\tentry\t1.5 400\n";
    ofs << "missing-weight\tentry\n";
    ofs << "bad-number\tentry\tnot-a-number 401\n";
    ofs << "valid\tsecond\t2.5 401\n";
  }

  azookey::learning::LearningStore loaded(path);
  EXPECT_TRUE(loaded.Load());
  EXPECT_EQ(loaded.size(), 2u);
  EXPECT_DOUBLE_EQ(loaded.Score("valid", "entry", 400), 1.5);
  EXPECT_DOUBLE_EQ(loaded.Score("valid", "second", 401), 2.5);

  std::remove(path.c_str());
}

TEST(LearningStoreTest, ScoreClampsClockRollbackToFullWeight) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "azookey_learning_clock_rollback.tsv").string();
  std::remove(path.c_str());

  azookey::learning::LearningStore store(path);
  store.Observe("にほん", "二本", 3.25, 1000);

  EXPECT_DOUBLE_EQ(store.Score("にほん", "二本", 1000), 3.25);
  EXPECT_DOUBLE_EQ(store.Score("にほん", "二本", 900), 3.25);

  std::remove(path.c_str());
}
TEST(LearningStoreTest, DirtyTracksSaveSuccessAndFailure) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_learning_dirty_test";
  const auto path = root / "learning.tsv";
  std::filesystem::remove_all(root);

  azookey::learning::LearningStore store(path.string());
  EXPECT_FALSE(store.dirty());
  store.Observe("reading", "surface", 1.0, 300);
  EXPECT_TRUE(store.dirty());
  EXPECT_EQ(store.size(), 1u);
  EXPECT_TRUE(store.Save());
  EXPECT_FALSE(store.dirty());

  azookey::learning::LearningStore loaded(path.string());
  EXPECT_TRUE(loaded.Load());
  EXPECT_FALSE(loaded.dirty());
  EXPECT_EQ(loaded.size(), 1u);

  const auto blocked_path = root / "blocked-directory";
  std::filesystem::create_directories(blocked_path);
  azookey::learning::LearningStore failing(blocked_path.string());
  failing.Observe("reading", "blocked", 1.0, 300);
  EXPECT_FALSE(failing.Save());
  EXPECT_TRUE(failing.dirty());

  std::filesystem::remove_all(root);
}

TEST(LearningStoreTest, PruneDropsLowScoresAndCapsRecordCount) {
  constexpr uint64_t kNow = 2'000'000'000;
  const std::string path =
      (std::filesystem::temp_directory_path() / "azookey_learning_prune_test.tsv").string();
  std::remove(path.c_str());

  azookey::learning::LearningStore store(path);
  store.Observe("r", "drop-low", 1.0, kNow);
  store.Observe("r", "keep-mid", 2.0, kNow);
  store.Observe("r", "keep-high", 3.0, kNow);

  store.Prune(/*max_records=*/2, /*min_weight=*/0.0, kNow);
  EXPECT_EQ(store.size(), 2u);
  EXPECT_DOUBLE_EQ(store.Score("r", "drop-low", kNow), 0.0);
  EXPECT_GT(store.Score("r", "keep-mid", kNow), 0.0);
  EXPECT_GT(store.Score("r", "keep-high", kNow), 0.0);

  store.Observe("r", "drop-decayed", 1.0, kNow - 365ull * 24ull * 60ull * 60ull);
  store.Prune(/*max_records=*/10, /*min_weight=*/0.05, kNow);
  EXPECT_DOUBLE_EQ(store.Score("r", "drop-decayed", kNow), 0.0);

  std::remove(path.c_str());
}
