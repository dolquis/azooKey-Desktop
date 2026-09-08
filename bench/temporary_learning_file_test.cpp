#include "TemporaryLearningFile.h"

#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace azookey::bench {
namespace {

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
    input.close();
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
  { std::ofstream output(first_path); output << "first"; }
  { std::ofstream output(second.Path()); output << "second"; }
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
    ~Writer() { std::ofstream output(path); output << "shutdown"; }
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

}  // namespace
}  // namespace azookey::bench
