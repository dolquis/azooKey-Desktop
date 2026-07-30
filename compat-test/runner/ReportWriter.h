#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "runner/CompatTypes.h"

namespace azookey::compat_test {

struct ReportSummary {
  size_t passed{0};
  size_t failed{0};
  size_t failing_skipped{0};
};

std::filesystem::path CreateTimestampedOutputDirectory();
bool PrepareOutputDirectory(const std::filesystem::path& output_directory);
ReportSummary SummarizeResults(const std::vector<CaseResult>& results);
bool WriteReports(const std::filesystem::path& output_directory, const TargetConfig& target,
                  const std::vector<CaseResult>& results);

}  // namespace azookey::compat_test
