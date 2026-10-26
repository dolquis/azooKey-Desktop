#pragma once

#include <memory>
#include <string>
#include <vector>

#include "azookey/core/CustomRomajiLoader.h"

namespace azookey::core {

class RomajiKanaConverter {
 public:
  std::string Feed(char ascii);
  std::string Flush();
  void Reset();
  // Set only at a composition boundary; copies retain their own table snapshot.
  // A null table uses the built-in romaji mappings.
  void SetCustomTable(std::shared_ptr<const CustomRomajiTable> table);
  bool HasCustomTable() const { return static_cast<bool>(custom_table_); }
  // Whether this ASCII character continues a rule or begins one after pending bytes flush.
  bool CanContinueCustomWith(char ascii) const;
  bool HasPending() const { return !pending_.empty(); }
  void PopPendingPreview();
  std::string PreviewPending() const;

  static std::string Preview(const std::string& ascii);
  static std::string Preview(const std::string& ascii,
                             std::shared_ptr<const CustomRomajiTable> table);
  static std::string ConvertForCommit(const std::string& ascii);
  static std::string ConvertForCommit(const std::string& ascii,
                                      std::shared_ptr<const CustomRomajiTable> table);

  static constexpr size_t kMaxAmbiguousDoubleN = 3;
  // Commit readings of `ascii` when each built-in "nn" + vowel/y is read either as
  // ん + な行 (the Feed default, always first) or as ん + あ行/や行. Only the first
  // `max_ambiguous` (at most kMaxAmbiguousDoubleN) such positions vary; later ones keep
  // the default. A position does not vary when reading it alone that way leaves a
  // different count of unconverted ASCII letters than the default (e.g. a trailing
  // "nny" or "nnyi"). A custom table has no ambiguous positions, so it yields its
  // single commit reading.
  static std::vector<std::string> AmbiguousReadings(const std::string& ascii,
                                                    size_t max_ambiguous = kMaxAmbiguousDoubleN);
  static std::vector<std::string> AmbiguousReadings(const std::string& ascii,
                                                    std::shared_ptr<const CustomRomajiTable> table,
                                                    size_t max_ambiguous = kMaxAmbiguousDoubleN);

 private:
  std::string pending_;
  std::shared_ptr<const CustomRomajiTable> custom_table_;
  // Bit i set: the i-th ambiguous "nn" + vowel/y consumes both n (used by AmbiguousReadings).
  unsigned consume_double_n_mask_ = 0;
  size_t ambiguous_double_n_seen_ = 0;
  std::string ConvertPending(bool force_flush);
  std::string ConvertCustomPending(bool force_flush);
};

}  // namespace azookey::core
