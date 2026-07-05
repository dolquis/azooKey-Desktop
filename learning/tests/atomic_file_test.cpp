#include "AtomicFile.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "azookey/learning/FileLock.h"

namespace {

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream ifs(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

size_t CountTempFiles(const std::filesystem::path& dir) {
  size_t temp_files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().filename().string().find(".tmp.") != std::string::npos) {
      ++temp_files;
    }
  }
  return temp_files;
}

TEST(AtomicFileTest, SuccessfulWritePersistsContentAndLeavesNoTempFiles) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_atomicfile_success";
  std::filesystem::remove_all(root);
  ASSERT_TRUE(std::filesystem::create_directories(root));
  const auto target = root / "store.tsv";

  ASSERT_TRUE(azookey::learning::WriteTextFileAtomically(target.string(), "reading\tsurface\n"));

  ASSERT_TRUE(std::filesystem::exists(target));
  EXPECT_EQ(ReadAll(target), "reading\tsurface\n");
  EXPECT_EQ(CountTempFiles(root), 0u);

  std::filesystem::remove_all(root);
}

TEST(AtomicFileTest, OverwriteReplacesExistingContentAndLeavesNoTempFiles) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_atomicfile_overwrite";
  std::filesystem::remove_all(root);
  ASSERT_TRUE(std::filesystem::create_directories(root));
  const auto target = root / "store.tsv";

  ASSERT_TRUE(azookey::learning::WriteTextFileAtomically(target.string(), "old\n"));
  ASSERT_TRUE(azookey::learning::WriteTextFileAtomically(target.string(), "new\n"));

  EXPECT_EQ(ReadAll(target), "new\n");
  EXPECT_EQ(CountTempFiles(root), 0u);

  std::filesystem::remove_all(root);
}

TEST(AtomicFileTest, UnwritableTargetReturnsFalseAndLeavesNoTempFiles) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_atomicfile_unwritable";
  std::filesystem::remove_all(root);
  ASSERT_TRUE(std::filesystem::create_directories(root));
  const auto blocking_parent = root / "not_a_directory";
  {
    std::ofstream out(blocking_parent, std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out << "blocking file";
  }
  const auto target = blocking_parent / "store.tsv";

  EXPECT_FALSE(azookey::learning::WriteTextFileAtomically(target.string(), "payload"));

  EXPECT_EQ(ReadAll(blocking_parent), "blocking file");
  EXPECT_EQ(CountTempFiles(root), 0u);

  std::filesystem::remove_all(root);
}

TEST(AtomicFileTest, RenameFailureRemovesStagedTempFile) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_atomicfile_rename_failure";
  std::filesystem::remove_all(root);
  ASSERT_TRUE(std::filesystem::create_directories(root));
  const auto target = root / "store.tsv";
  ASSERT_TRUE(std::filesystem::create_directories(target));

  EXPECT_FALSE(azookey::learning::WriteTextFileAtomically(target.string(), "payload"));

  EXPECT_TRUE(std::filesystem::is_directory(target));
  EXPECT_EQ(CountTempFiles(root), 0u);

  std::filesystem::remove_all(root);
}

// Concurrent writers targeting the same path must each stage their own temp
// file and atomically rename it in. If two writers shared a temp name (the
// pre-fix behavior when their clock readings collided), they would truncate and
// interleave the same file, so the final content would match no single writer.
TEST(AtomicFileTest, ConcurrentWritesToSamePathDoNotCorruptOrLeakTempFiles) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_atomicfile_concurrent";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto target = root / "store.tsv";

  constexpr int kThreads = 16;
  std::vector<std::string> contents;
  contents.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    // Sizable, distinct payloads so an interleaved write cannot accidentally
    // equal any single writer's full content.
    contents.push_back(std::string(4096, static_cast<char>('A' + i)) + std::to_string(i));
  }

  std::vector<std::thread> threads;
  std::vector<char> ok(kThreads, 0);
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i] {
      ok[static_cast<size_t>(i)] =
          azookey::learning::WriteTextFileAtomically(target.string(), contents[static_cast<size_t>(i)])
              ? 1
              : 0;
    });
  }
  for (auto& t : threads) t.join();

  // At least one writer must land. We deliberately do NOT require every writer
  // to succeed: atomically replacing a single shared target under contention
  // can legitimately fail for some writers on Windows, where MoveFileEx may
  // report a sharing/access violation while another replace is in flight (POSIX
  // rename has no such restriction). The guarantee this fix provides is staging
  // isolation — each writer renames its *own* uniquely named temp — which the
  // corruption and temp-leak checks below verify regardless of who wins.
  const auto successes = std::count(ok.begin(), ok.end(), static_cast<char>(1));
  EXPECT_GE(successes, 1) << "no writer succeeded";

  ASSERT_TRUE(std::filesystem::exists(target));
  const std::string final_content = ReadAll(target);
  const std::set<std::string> valid(contents.begin(), contents.end());
  EXPECT_EQ(valid.count(final_content), 1u)
      << "final file content matches no single writer (interleaving/corruption)";
  EXPECT_EQ(CountTempFiles(root), 0u);

  std::filesystem::remove_all(root);
}

// Rapid successive writes used to be able to derive the same temp name from a
// coarse monotonic clock; the atomic sequence makes each name unique, so every
// write renames its own temp away and none are left behind.
TEST(AtomicFileTest, RapidSuccessiveWritesLeaveNoTempFiles) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_atomicfile_rapid";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto target = root / "store.tsv";

  for (int i = 0; i < 256; ++i) {
    ASSERT_TRUE(
        azookey::learning::WriteTextFileAtomically(target.string(), "payload" + std::to_string(i)));
  }

  EXPECT_EQ(ReadAll(target), "payload255");
  EXPECT_EQ(CountTempFiles(root), 0u);

  std::filesystem::remove_all(root);
}

TEST(AtomicFileTest, FileLockExcludesSamePathAcrossThreadsOnWindows) {
#ifndef _WIN32
  GTEST_SKIP() << "The production cross-process lock is a Windows named mutex.";
#else
  const auto root = std::filesystem::temp_directory_path() / "azookey_atomicfile_lock";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto target = root / "user_dict.json";

  auto first =
      azookey::learning::AcquireExclusiveFileLockForPath(target, std::chrono::milliseconds(100));
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(first->owns_lock());

  auto blocked = std::async(std::launch::async, [&] {
    return azookey::learning::AcquireExclusiveFileLockForPath(target, std::chrono::milliseconds(50))
        .has_value();
  });
  EXPECT_FALSE(blocked.get());

  first.reset();
  auto second =
      azookey::learning::AcquireExclusiveFileLockForPath(target, std::chrono::milliseconds(100));
  EXPECT_TRUE(second.has_value());

  std::filesystem::remove_all(root);
#endif
}

}  // namespace
