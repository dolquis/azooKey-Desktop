#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::settings {

// The stores the "学習" pane lists, in tab order (learning-data-management-spec section 3).
// `id` is the wire name of the `store` field; `resource` names the tab title string.
struct LearningStoreTab {
  std::string_view id;
  std::string_view resource;
};

inline constexpr std::array<LearningStoreTab, 4> kLearningStoreTabs{{
    {"learning", "LearningStore_learning"},
    {"user_dict", "LearningStore_user_dict"},
    {"typo", "LearningStore_typo"},
    {"auto_word", "LearningStore_auto_word"},
}};

inline constexpr uint32_t kLearningPageSize = 100;

// `conflict_resolution` values of ImportLearningData, in the order the pane offers them.
inline constexpr std::array<std::string_view, 3> kConflictResolutions{"merge", "overwrite",
                                                                      "keep_both"};

// Error categories the four responses carry (learning-data-management-spec section 4.5), the
// payload constants in ipc/Payloads.h, and the BackupErrorCode names of archive failures. Each
// has a "LearningError_<code>" string; any other falls back to "LearningError_other".
inline constexpr std::array<std::string_view, 17> kLearningErrorCodes{
    "invalid_request",     "not_authenticated", "store_unavailable",
    "save_failed",         "unsupported",       "invalid_path",
    "destination_exists",  "source_missing",    "io",
    "too_large",           "not_archive",       "bad_manifest",
    "unsupported_version", "missing_item",      "checksum_mismatch",
    "decrypt_failed",      "bad_item"};

std::string LearningErrorResource(std::string_view error);

// Local date of a last-used time as "YYYY-MM-DD"; empty for 0 (never).
std::string FormatLearningDate(uint64_t epoch_seconds);

// Weight with one decimal.
std::string FormatLearningWeight(double weight);

// Tags joined with ", ".
std::string JoinLearningTags(const std::vector<std::string>& tags);

// The rows `offset`.. of `total` that a page holds, 1-based and inclusive, for "1-100 / 1234".
struct LearningPageRange {
  uint64_t first{};
  uint64_t last{};
  bool has_previous{false};
  bool has_next{false};
};
LearningPageRange ComputeLearningPage(uint64_t offset, uint64_t shown, uint64_t total);

// A backup archive path ExportLearningData accepts: absolute and ending in ".zip".
bool IsBackupArchivePath(std::string_view utf8_path);

}  // namespace azookey::settings
