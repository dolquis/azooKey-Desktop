#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
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
