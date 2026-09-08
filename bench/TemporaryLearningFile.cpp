#include "TemporaryLearningFile.h"

#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

namespace azookey::bench {

TemporaryLearningFile::TemporaryLearningFile() {
  const auto root = std::filesystem::temp_directory_path();
  std::random_device random;
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto directory =
        root / ("azookey-bench-" + std::to_string(random()) + "-" + std::to_string(random()));
    // Prepare both paths before acquiring ownership; allocation failures cannot leak it.
    directory_ = directory;
    path_ = directory / "learning.tsv";
    // Retry name collisions only; filesystem errors propagate immediately.
    if (std::filesystem::create_directory(directory_)) return;
  }
  throw std::runtime_error("failed to reserve a benchmark learning directory");
}

TemporaryLearningFile::~TemporaryLearningFile() {
  std::error_code file_error;
  std::filesystem::remove(path_, file_error);
  // Do not recursively delete unexpected files, even inside our reserved directory.
  std::error_code directory_error;
  std::filesystem::remove(directory_, directory_error);
  if (file_error || directory_error) {
    // C stdio keeps diagnostics non-throwing, even during exception unwinding.
    std::fprintf(stderr, "benchmark learning cleanup failed: file_error=%d directory_error=%d\n",
                 file_error.value(), directory_error.value());
  }
}

}  // namespace azookey::bench
