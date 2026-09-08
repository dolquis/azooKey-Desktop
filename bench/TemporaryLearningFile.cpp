#include "TemporaryLearningFile.h"

#include <random>
#include <stdexcept>
#include <string>

namespace azookey::bench {

TemporaryLearningFile::TemporaryLearningFile() {
  const auto root = std::filesystem::temp_directory_path();
  std::random_device random;
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto directory = root / ("azookey-bench-" + std::to_string(random()) + "-" +
                                   std::to_string(random()));
    // Prepare both paths before acquiring ownership; allocation failures cannot leak it.
    directory_ = directory;
    path_ = directory / "learning.tsv";
    if (std::filesystem::create_directory(directory_)) return;
  }
  throw std::runtime_error("failed to reserve a benchmark learning directory");
}

TemporaryLearningFile::~TemporaryLearningFile() {
  std::error_code error;
  std::filesystem::remove(path_, error);
  // Do not recursively delete unexpected files, even inside our reserved directory.
  std::filesystem::remove(directory_, error);
}

}  // namespace azookey::bench
