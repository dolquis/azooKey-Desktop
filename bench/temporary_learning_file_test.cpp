#include <gtest/gtest.h>

#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "TemporaryLearningFile.h"

namespace azookey::bench {
namespace {

static_assert(!std::is_copy_constructible_v<TemporaryLearningFile>);
static_assert(!std::is_move_constructible_v<TemporaryLearningFile>);

TEST(TemporaryLearningFileTest, ConcurrentReservationsKeepIndependentFiles) {
  std::vector<std::future<std::unique_ptr<TemporaryLearningFile>>> pending;
  for (int i = 0; i < 8; ++i) {
    pending.push_back(std::async(std::launch::async, [i] {
      auto file = std::make_unique<TemporaryLearningFile>();
      std::ofstream output(file->Path());
      output << i;
      return file;
    }));
  }
  std::vector<std::unique_ptr<TemporaryLearningFile>> files;
  for (auto& future : pending) files.push_back(future.get());
  for (size_t i = 0; i < files.size(); ++i) {
    std::ifstream input(files[i]->Path());
    int contents = -1;
    input >> contents;
    EXPECT_EQ(contents, static_cast<int>(i));
    input.close();  // Windows cannot delete the file while this stream holds it open.
    const auto directory = files[i]->Path().parent_path();
    files[i].reset();
    EXPECT_FALSE(std::filesystem::exists(directory));
  }
}

TEST(TemporaryLearningFileTest, CleanupDoesNotTouchAnotherLiveOwner) {
  auto first = std::make_unique<TemporaryLearningFile>();
  const auto first_path = first->Path();
  TemporaryLearningFile second;
  ASSERT_NE(first_path.parent_path(), second.Path().parent_path());
  ASSERT_FALSE(std::filesystem::exists(first_path));
  ASSERT_FALSE(std::filesystem::exists(second.Path()));
  {
    std::ofstream output(first_path);
    output << "first";
  }
  {
    std::ofstream output(second.Path());
    output << "second";
  }
  first.reset();
  EXPECT_FALSE(std::filesystem::exists(first_path.parent_path()));
  std::ifstream input(second.Path());
  std::string contents;
  input >> contents;
  EXPECT_EQ(contents, "second");
}

TEST(TemporaryLearningFileTest, CleanupRunsAfterLaterObjectsDuringUnwinding) {
  struct Writer {
    std::filesystem::path path;
    ~Writer() {
      std::filesystem::create_directories(path.parent_path());
      std::ofstream output(path);
      output << "shutdown";
    }
  };
  std::filesystem::path directory;
  try {
    TemporaryLearningFile temporary;
    directory = temporary.Path().parent_path();
    Writer writer{temporary.Path()};
    throw 1;
  } catch (int) {
  }
  EXPECT_FALSE(std::filesystem::exists(directory));
}

TEST(TemporaryLearningFileTest, CleanupPreservesUnexpectedFilesAndReportsFailure) {
  auto owner = std::make_unique<TemporaryLearningFile>();
  const auto directory = owner->Path().parent_path();
  const auto unexpected = directory / "unexpected.txt";
  {
    std::ofstream output(unexpected);
    output << "preserve";
  }
  testing::internal::CaptureStderr();
  owner.reset();
  const auto diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(std::filesystem::exists(directory));
  EXPECT_TRUE(std::filesystem::exists(unexpected));
  EXPECT_NE(diagnostic.find("benchmark learning cleanup failed:"), std::string::npos);
  // Remove only the test-owned file and now-empty directory.
  EXPECT_TRUE(std::filesystem::remove(unexpected));
  EXPECT_TRUE(std::filesystem::remove(directory));
}

}  // namespace
}  // namespace azookey::bench
