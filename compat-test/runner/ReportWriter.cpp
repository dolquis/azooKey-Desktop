#include "runner/ReportWriter.h"

#include <Windows.h>

#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <system_error>

#include "azookey/ipc/Json.h"

namespace azookey::compat_test {
namespace {

std::string SafeReasonCode(std::string_view reason) {
  if (reason.empty()) return "none";
  for (const unsigned char ch : reason) {
    if (!(std::islower(ch) || std::isdigit(ch) || ch == '-')) {
      return "redacted-detail";
    }
  }
  return std::string(reason);
}

std::string RelativeFailurePath(const std::filesystem::path& output_directory,
                                const std::filesystem::path& failure_directory) {
  if (failure_directory.empty()) return {};
  std::error_code ec;
  const auto relative = std::filesystem::relative(failure_directory, output_directory, ec);
  return ec ? std::string{} : relative.generic_string();
}

std::string TimestampForDirectory() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_s(&local, &time);
  std::ostringstream out;
  out << std::put_time(&local, "%Y%m%d-%H%M%S");
  return out.str();
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) return false;
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  return stream.good();
}

bool EnsureOutputSubdirectories(const std::filesystem::path& output_directory) {
  std::error_code ec;
  std::filesystem::create_directories(output_directory / "screenshots", ec);
  if (ec) return false;
  std::filesystem::create_directories(output_directory / "failures", ec);
  return !ec;
}

}  // namespace

const char* ResultStatusName(ResultStatus status) {
  switch (status) {
    case ResultStatus::Pass:
      return "pass";
    case ResultStatus::Fail:
      return "fail";
    case ResultStatus::FailingSkip:
      return "failing-skip";
  }
  return "failing-skip";
}

std::filesystem::path CreateTimestampedOutputDirectory() {
  return std::filesystem::current_path() / ("compat-report-" + TimestampForDirectory());
}

bool PrepareOutputDirectory(const std::filesystem::path& output_directory) {
  std::error_code ec;
  if (std::filesystem::exists(output_directory, ec)) {
    if (ec || !std::filesystem::is_directory(output_directory, ec) || ec) return false;
    if (std::filesystem::directory_iterator(output_directory, ec) !=
        std::filesystem::directory_iterator()) {
      return false;
    }
    if (ec) return false;
  }
  return EnsureOutputSubdirectories(output_directory);
}

ReportSummary SummarizeResults(const std::vector<CaseResult>& results) {
  ReportSummary summary;
  for (const auto& result : results) {
    switch (result.status) {
      case ResultStatus::Pass:
        ++summary.passed;
        break;
      case ResultStatus::Fail:
        ++summary.failed;
        break;
      case ResultStatus::FailingSkip:
        ++summary.failing_skipped;
        break;
    }
  }
  return summary;
}

bool WriteReports(const std::filesystem::path& output_directory, const TargetConfig& target,
                  const std::vector<CaseResult>& results) {
  if (!EnsureOutputSubdirectories(output_directory)) return false;

  const auto summary = SummarizeResults(results);
  azookey::ipc::json::Array json_results;
  std::ostringstream markdown;
  markdown << "# Compatibility test report\n\n"
           << "- Target: `" << target.id << "`\n"
           << "- Automation: `" << target.automation_level << "`\n"
           << "- Pass: " << summary.passed << "\n"
           << "- Fail: " << summary.failed << "\n"
           << "- Failing skip: " << summary.failing_skipped << "\n\n"
           << "| Case | Result | Reason | Duration (ms) | Artifact |\n"
           << "|---|---|---|---:|---|\n";

  for (const auto& result : results) {
    const std::string reason = SafeReasonCode(result.reason_code);
    const std::string artifact = RelativeFailurePath(output_directory, result.failure_directory);
    azookey::ipc::json::Object item;
    item.emplace("id", result.id);
    item.emplace("status", ResultStatusName(result.status));
    item.emplace("reason_code", reason);
    item.emplace("duration_ms", result.duration_ms);
    if (!artifact.empty()) item.emplace("artifact", artifact);
    json_results.emplace_back(std::move(item));

    markdown << "| " << result.id << " | " << ResultStatusName(result.status) << " | " << reason
             << " | " << result.duration_ms << " | ";
    if (!artifact.empty()) markdown << "`" << artifact << "`";
    markdown << " |\n";
  }

  azookey::ipc::json::Object json_summary;
  json_summary.emplace("pass", static_cast<uint64_t>(summary.passed));
  json_summary.emplace("fail", static_cast<uint64_t>(summary.failed));
  json_summary.emplace("failing_skip", static_cast<uint64_t>(summary.failing_skipped));

  azookey::ipc::json::Object json_target;
  json_target.emplace("id", target.id);
  json_target.emplace("display_name", target.display_name);
  json_target.emplace("app_id", target.app_id);
  json_target.emplace("automation_level", target.automation_level);

  azookey::ipc::json::Object root;
  root.emplace("schema_version", 1);
  root.emplace("target", std::move(json_target));
  root.emplace("summary", std::move(json_summary));
  root.emplace("results", std::move(json_results));

  const std::string json =
      azookey::ipc::json::Stringify(azookey::ipc::json::Value(std::move(root))) + "\n";
  return WriteTextFile(output_directory / "report.json", json) &&
         WriteTextFile(output_directory / "report.md", markdown.str());
}

}  // namespace azookey::compat_test
