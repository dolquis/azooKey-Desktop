#pragma once

#include <memory>
#include <string>

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
  bool HasPending() const { return !pending_.empty(); }
  void PopPendingPreview();
  std::string PreviewPending() const;

  static std::string Preview(const std::string& ascii);
  static std::string Preview(const std::string& ascii,
                             std::shared_ptr<const CustomRomajiTable> table);
  static std::string ConvertForCommit(const std::string& ascii);
  static std::string ConvertForCommit(const std::string& ascii,
                                      std::shared_ptr<const CustomRomajiTable> table);

 private:
  std::string pending_;
  std::shared_ptr<const CustomRomajiTable> custom_table_;
  std::string ConvertPending(bool force_flush);
  std::string ConvertCustomPending(bool force_flush);
};

}  // namespace azookey::core
