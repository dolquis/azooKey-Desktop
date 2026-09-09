#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>

#include "azookey/core/CrashRetention.h"

namespace {
namespace fs = std::filesystem;

class CrashRetentionTest : public testing::Test {
 protected:
  fs::path directory;
  fs::file_time_type now = fs::file_time_type::clock::now();

  void SetUp() override {
    for (int i = 0; i < 100; ++i) {
      directory = fs::temp_directory_path() /
                  ("azookey-crash-retention-test-" + std::to_string(std::random_device{}()));
      if (fs::create_directory(directory)) return;
    }
    FAIL() << "Unable to create unique test directory";
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(directory, ec);
  }
  fs::path Dump(int pid, std::size_t size, int age_hours, bool settings = false) {
    const auto path = directory / ((settings ? "azookey-settings-" : "azookey-host-") +
                                   std::string("20260909T000000Z-") + std::to_string(pid) + ".dmp");
    std::ofstream(path, std::ios::binary) << std::string(size, 'x');
    fs::last_write_time(path, now - std::chrono::hours(age_hours));
    return path;
  }
};

TEST_F(CrashRetentionTest, CountAppliesAcrossModules) {
  for (int i = 0; i < 7; ++i) Dump(i, 2, i, i % 2 == 0);
  const auto result = azookey::core::PruneCrashDumps(directory, {}, now);
  EXPECT_FALSE(result.failed);
  EXPECT_EQ(result.removed, 2u);
  EXPECT_EQ(std::distance(fs::directory_iterator(directory), fs::directory_iterator{}), 5);
}

TEST_F(CrashRetentionTest, SizeDropsOldestFirst) {
  auto newest = Dump(1, 6, 0);
  auto oldest = Dump(2, 6, 1);
  auto result = azookey::core::PruneCrashDumps(directory, {5, 10, std::chrono::hours(720)}, now);
  EXPECT_FALSE(result.failed);
  EXPECT_TRUE(fs::exists(newest));
  EXPECT_FALSE(fs::exists(oldest));
}

TEST_F(CrashRetentionTest, OversizedNewestCannotRetainOlderFiles) {
  Dump(1, 11, 0);
  Dump(2, 1, 1);
  auto result = azookey::core::PruneCrashDumps(directory, {5, 10, std::chrono::hours(720)}, now);
  EXPECT_FALSE(result.failed);
  EXPECT_EQ(result.removed, 2u);
}

TEST_F(CrashRetentionTest, AgeBoundaryAndUnmanagedFiles) {
  auto boundary = Dump(1, 1, 720);
  auto expired = Dump(2, 1, 721);
  const auto unrelated = directory / u8"メモ-📝.dmp";
  std::ofstream(unrelated) << "preserve";
  const auto malformed = directory / "azookey-host-not-a-timestamp-2.dmp";
  std::ofstream(malformed) << "preserve";
  auto result = azookey::core::PruneCrashDumps(directory, {}, now);
  EXPECT_FALSE(result.failed);
  EXPECT_TRUE(fs::exists(boundary));
  EXPECT_FALSE(fs::exists(expired));
  EXPECT_TRUE(fs::exists(unrelated));
  EXPECT_TRUE(fs::exists(malformed));
}

TEST_F(CrashRetentionTest, MissingDirectoryIsNotCreatedAndFileIsSafeFailure) {
  const auto missing = directory / "missing";
  EXPECT_FALSE(azookey::core::PruneCrashDumps(missing).failed);
  EXPECT_FALSE(fs::exists(missing));
  const auto file = directory / "file";
  std::ofstream(file) << "preserve";
  EXPECT_TRUE(azookey::core::PruneCrashDumps(file).failed);
  EXPECT_TRUE(fs::exists(file));
}

TEST_F(CrashRetentionTest, DirectoryAndSymlinkAreNeverDeleted) {
  const auto nested = directory / "azookey-host-20260909T000000Z-1.dmp";
  fs::create_directory(nested);
  const auto target = Dump(2, 1, 0);
  const auto link = directory / "azookey-host-20260909T000000Z-3.dmp";
  std::error_code ec;
  fs::create_symlink(target, link, ec);
  const bool linked = !ec;
  const auto result = azookey::core::PruneCrashDumps(directory, {}, now);
  EXPECT_FALSE(result.failed);
  EXPECT_TRUE(fs::is_directory(nested));
  EXPECT_TRUE(fs::exists(target));
  if (linked) EXPECT_TRUE(fs::is_symlink(link));
}

}  // namespace
