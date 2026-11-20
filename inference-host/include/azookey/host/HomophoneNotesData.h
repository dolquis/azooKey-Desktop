#pragma once

#include <filesystem>
#include <memory>
#include <mutex>

#include "azookey/core/HomophoneNotes.h"
#include "azookey/logging/RuntimeLogger.h"

namespace azookey::host {
// docs/homophone-notes-spec.md section 7. One instance is shared by every
// connection; the caller asks only while homophoneNotes is true, so the data is
// not read while it is false.
class HomophoneNotesData {
 public:
  // An empty path is data/homophone-notes.tsv next to the Host executable.
  explicit HomophoneNotesData(std::filesystem::path path = {});

  // Loads on the first call and caches the outcome, success or failure, so a
  // failure is warned once. nullptr when the file cannot be read; rows that
  // break the grammar are dropped and counted in the warning.
  std::shared_ptr<const core::HomophoneNoteIndex> Get(logging::RuntimeLogger* logger);

 private:
  std::filesystem::path path_;
  std::mutex mutex_;
  bool attempted_{false};
  std::shared_ptr<const core::HomophoneNoteIndex> index_;
};
}  // namespace azookey::host
