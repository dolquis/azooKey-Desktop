#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/ipc/Payloads.h"

namespace azookey::diagnostics {

enum class Status {
  Ok,
  Warning,
  Error,
};

struct Check {
  std::string id;
  std::string name;
  Status status{Status::Ok};
  std::string message;
  std::string details_json{"{}"};
};

struct Report {
  Status status{Status::Ok};
  uint64_t timestamp_ms{};
  std::vector<Check> checks;
};

struct Snapshot {
  bool tip_path_registered{false};
  bool tip_path_exists{false};
  bool tip_bitness_matches{false};
  std::string tip_path;
  bool com_registration_matches{false};
  bool language_profile_registered{false};
  bool host_running{false};
  bool handshake_ok{false};
  bool ping_responded{false};
  uint64_t ping_rtt_ms{};
  bool model_enabled{true};
  std::string selected_model_path;
  bool selected_model_exists{false};
  bool selected_model_valid{false};
  bool settings_valid{true};
  bool settings_missing{true};
  bool learning_store_valid{true};
  bool user_dict_valid{true};
  uint64_t learning_entries{};
  uint64_t user_dict_entries{};
  uint64_t user_dict_skipped_entries{};
  bool logs_directory_writable{false};
  std::optional<ipc::QueryDiagnosticsPayload> host_diagnostics;
};

struct ProbeResult {
  Report report;
  std::filesystem::path settings_path;
  std::filesystem::path tip_path;
  std::filesystem::path logs_directory;
  std::string host_health_json;
  std::string ipc_ping_json;
};

enum class RepairStatus {
  Succeeded,
  Failed,
  NotNeeded,
  PermissionDenied,
};

struct RepairOperationResult {
  RepairStatus status{RepairStatus::Failed};
  std::string message;
};

struct RepairResult {
  std::string id;
  RepairStatus status{RepairStatus::Failed};
  Status before_status{Status::Error};
  std::optional<Status> after_status;
  std::string message;
};

struct RepairReport {
  Report report;
  std::vector<RepairResult> repairs;
  bool probe_failed{false};
};

struct RepairHooks {
  std::function<ProbeResult()> probe_system;
  std::function<RepairOperationResult(const ProbeResult&)> repair_registration;
  std::function<RepairOperationResult(const std::filesystem::path&)> repair_logs_directory;
};

struct ArchiveEntry {
  std::string name;
  std::string content;
};

Report EvaluateSnapshot(const Snapshot& snapshot, uint64_t timestamp_ms);
std::string SerializeReport(const Report& report);
std::string StatusName(Status status);

ProbeResult ProbeSystem();
RepairReport RepairSystem();
RepairReport RepairSystem(const RepairHooks& hooks);
std::string SerializeRepairReport(const RepairReport& report);
std::string RepairStatusName(RepairStatus status);
bool RepairReportSucceeded(const RepairReport& report);

bool ProbeSettingsFile(const std::filesystem::path& path);
bool ProbeLearningStoreFile(const std::filesystem::path& path, uint64_t* entries);
bool ProbeUserDictionaryFile(const std::filesystem::path& path, uint64_t* entries,
                             uint64_t* skipped_entries);
bool EmbeddedSettingsSchemaUsesOnlySupportedKeywords();
std::filesystem::path RuntimeLogsDirectory();
bool ProbeLogsDirectory(const std::filesystem::path& path);
bool RepairLogsDirectory(const std::filesystem::path& path, std::string* error);

std::string RedactSettingsJson(std::string_view json);
std::string RedactFreeText(std::string text);
std::vector<ArchiveEntry> BuildCollectionEntries(const ProbeResult& result);
bool WriteZip(const std::filesystem::path& output, const std::vector<ArchiveEntry>& entries,
              std::string* error);

}  // namespace azookey::diagnostics
