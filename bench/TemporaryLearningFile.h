#pragma once

#include <filesystem>

namespace azookey::bench {

// Declare before LearningStore and InferenceEngine so cleanup follows their shutdown.
class TemporaryLearningFile {
 public:
  TemporaryLearningFile();
  ~TemporaryLearningFile();
  TemporaryLearningFile(const TemporaryLearningFile&) = delete;
  TemporaryLearningFile& operator=(const TemporaryLearningFile&) = delete;
  TemporaryLearningFile(TemporaryLearningFile&&) = delete;
  TemporaryLearningFile& operator=(TemporaryLearningFile&&) = delete;

  const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path directory_;
  std::filesystem::path path_;
};

}  // namespace azookey::bench
