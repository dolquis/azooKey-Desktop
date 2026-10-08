#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace azookey::learning {

// learning-data-management-spec section 6. A key that exists only in the
// imported data is always added; the policy decides what happens on a clash.
enum class ImportConflictPolicy {
  // Counts and weights are added; the existing entry wins where adding has no
  // meaning (user dictionary words).
  Merge,
  // The imported entry replaces the existing one.
  Overwrite,
  // The existing entry is kept. The stores key entries uniquely, so "keep
  // both" cannot hold two values under one key.
  KeepBoth,
};

std::string_view ImportConflictPolicyName(ImportConflictPolicy policy);
std::optional<ImportConflictPolicy> ParseImportConflictPolicy(std::string_view name);

struct ImportCounts {
  size_t imported{};
  size_t skipped{};
  size_t conflicts{};
};

}  // namespace azookey::learning
