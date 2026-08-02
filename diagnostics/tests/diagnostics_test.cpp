#include "Diagnostics.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "azookey/ipc/Json.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/UserDictionary.h"
#include "azookey/logging/RuntimeLogger.h"

namespace {

namespace diag = ::azookey::diagnostics;
namespace j = ::azookey::ipc::json;

std::filesystem::path TempPath(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

TEST(DiagnosticsTest, JsonSchemaSnapshotIsStableAndMockCannotReportLoadedModel) {
  diag::Snapshot snapshot;
  snapshot.tip_path_registered = true;
  snapshot.tip_path_exists = true;
  snapshot.tip_bitness_matches = true;
  snapshot.tip_path = R"(C:\Program Files\azooKey\azookey_tsf_tip.dll)";
  snapshot.com_registration_matches = true;
  snapshot.language_profile_registered = true;
  snapshot.host_running = true;
  snapshot.handshake_ok = true;
  snapshot.ping_responded = true;
  snapshot.ping_rtt_ms = 250;
  snapshot.model_enabled = true;
  snapshot.selected_model_path = R"(C:\Models\model.gguf)";
  snapshot.selected_model_exists = true;
  snapshot.selected_model_valid = true;
  snapshot.settings_valid = true;
  snapshot.settings_missing = false;
  snapshot.learning_store_valid = true;
  snapshot.user_dict_valid = true;
  snapshot.learning_entries = 2;
  snapshot.user_dict_entries = 3;
  snapshot.logs_directory_writable = true;
  azookey::ipc::QueryDiagnosticsPayload host;
  host.model_loaded = true;
  host.loaded_model_path = snapshot.selected_model_path;
  host.engine = "mock";
  host.backend = "cpu";
  host.rss_mb = 42;
  host.learning_entries = 2;
  host.user_dict_entries = 3;
  host.fallback_state = "degraded_simple";
  snapshot.host_diagnostics = host;

  const auto actual = diag::SerializeReport(diag::EvaluateSnapshot(snapshot, 1780000000000ULL));
  const auto expected = j::Parse(R"json(
{
  "status": "warning",
  "timestamp_ms": 1780000000000,
  "checks": [
    {
      "id": "D-001",
      "name": "tip_dll",
      "status": "ok",
      "message": "TIP DLL is present and matches process bitness",
      "details": {
        "bitness_matches": true,
        "exists": true,
        "path": "C:\\Program Files\\azooKey\\azookey_tsf_tip.dll",
        "registered": true
      }
    },
    {
      "id": "D-002",
      "name": "com_registration",
      "status": "ok",
      "message": "COM and profile registration match",
      "details": {"matches": true}
    },
    {
      "id": "D-003",
      "name": "language_profile",
      "status": "ok",
      "message": "Japanese language profile is registered",
      "details": {"langid": "0x0411", "registered": true}
    },
    {
      "id": "D-004",
      "name": "host_process",
      "status": "ok",
      "message": "Inference Host process is running",
      "details": {"running": true}
    },
    {
      "id": "D-005",
      "name": "ipc_handshake",
      "status": "ok",
      "message": "IPC handshake is ready",
      "details": {"ready": true}
    },
    {
      "id": "D-006",
      "name": "ipc_ping",
      "status": "warning",
      "message": "IPC ping latency exceeds the preferred threshold",
      "details": {"responded": true, "rtt_ms": 250}
    },
    {
      "id": "D-007",
      "name": "model_path",
      "status": "ok",
      "message": "Configured model path exists",
      "details": {
        "enabled": true,
        "exists": true,
        "path": "C:\\Models\\model.gguf",
        "selected": true
      }
    },
    {
      "id": "D-008",
      "name": "model_validation",
      "status": "warning",
      "message": "Selected model is valid but the effective runtime is mock",
      "details": {"engine": "mock", "loaded": false, "valid": true}
    },
    {
      "id": "D-009",
      "name": "fallback_state",
      "status": "warning",
      "message": "Runtime is operating in degraded mode",
      "details": {"state": "degraded_simple"}
    },
    {
      "id": "D-010",
      "name": "learning_store",
      "status": "ok",
      "message": "Learning store is readable",
      "details": {"entries": 2}
    },
    {
      "id": "D-011",
      "name": "user_dictionary",
      "status": "ok",
      "message": "User dictionary is readable",
      "details": {"entries": 3, "skipped_entries": 0}
    },
    {
      "id": "D-012",
      "name": "settings",
      "status": "ok",
      "message": "Settings conform to the embedded schema",
      "details": {"missing": false, "valid": true}
    },
    {
      "id": "D-013",
      "name": "logs",
      "status": "ok",
      "message": "Runtime logs directory is writable",
      "details": {"writable": true}
    }
  ]
}
)json");
  ASSERT_TRUE(expected.has_value());
  EXPECT_EQ(actual, j::Stringify(*expected));
}

TEST(DiagnosticsTest, CollectionSnapshotExcludesSensitiveBodies) {
  const auto settings_path = TempPath("azookey-diag-settings-redaction.json");
  const auto logs_directory = TempPath("azookey-diag-runtime-logs");
  std::error_code ec;
  std::filesystem::remove_all(logs_directory, ec);
  ASSERT_TRUE(std::filesystem::create_directories(logs_directory));
  {
    std::ofstream output(settings_path);
    output << R"({"openAiApiKey":"sk-secret","promptPrefixByApp":{"app":"private prompt"},)"
              R"("model":{"enabled":true,"selectedPath":"C:/Users/alice/model.gguf"}})";
  }
  diag::ProbeResult result;
  result.settings_path = settings_path;
  result.logs_directory = logs_directory;
  result.report.timestamp_ms = 1;
  result.report.checks.push_back(
      {"D-001", "tip_dll", diag::Status::Ok, "ok", R"({"token":"sk-diagnostic"})"});
  result.host_health_json =
      R"({"last_error":"Bearer private-token at D:\\Users\\bob\\model.gguf"})";
  result.ipc_ping_json = R"({"status":"ok","rtt_ms":1})";
  {
    std::ofstream output(logs_directory / "host-20260802.jsonl");
    output << R"({"event":"safe","api_key":"sk-runtime-secret","count":1,)"
              R"("candidate":"private candidate body","reading":"private reading body"})"
           << '\n';
    output << "invalid private input body\n";
  }
  {
    std::ofstream output(logs_directory / "host-20260802.jsonl.1");
    output << R"({"event":"rotated","surface":"private rotated surface"})" << '\n';
  }
  const auto old_log = logs_directory / "tip-20260701.jsonl";
  {
    std::ofstream output(old_log);
    output << R"({"event":"too_old_to_collect"})" << '\n';
  }
  std::filesystem::last_write_time(
      old_log, std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 8));

  const auto entries = diag::BuildCollectionEntries(result);
  const std::vector<std::string> expected_names = {
      "diag.json",       "settings.redacted.json",   "host-health.json",
      "ipc-ping.json",   "logs/host-20260802.jsonl", "logs/host-20260802.jsonl.1",
      "environment.txt", "crash-summary.txt",
  };
  ASSERT_EQ(entries.size(), expected_names.size());
  for (size_t index = 0; index < entries.size(); ++index) {
    EXPECT_EQ(entries[index].name, expected_names[index]);
  }
  std::string combined;
  for (const auto& entry : entries) combined += entry.content;
  EXPECT_EQ(combined.find("sk-secret"), std::string::npos);
  EXPECT_EQ(combined.find("sk-diagnostic"), std::string::npos);
  EXPECT_EQ(combined.find("private-token"), std::string::npos);
  EXPECT_EQ(combined.find("private prompt"), std::string::npos);
  EXPECT_EQ(combined.find("sk-runtime-secret"), std::string::npos);
  EXPECT_EQ(combined.find("private candidate body"), std::string::npos);
  EXPECT_EQ(combined.find("private reading body"), std::string::npos);
  EXPECT_EQ(combined.find("private rotated surface"), std::string::npos);
  EXPECT_EQ(combined.find("private input body"), std::string::npos);
  EXPECT_EQ(combined.find("too_old_to_collect"), std::string::npos);
  EXPECT_EQ(combined.find("alice"), std::string::npos);
  EXPECT_EQ(combined.find("bob"), std::string::npos);
  EXPECT_NE(combined.find("***redacted***"), std::string::npos);
  EXPECT_NE(combined.find("%USERPROFILE%"), std::string::npos);

  std::filesystem::remove(settings_path, ec);
  std::filesystem::remove_all(logs_directory, ec);
}

TEST(DiagnosticsTest, CollectionBoundsEachRuntimeLogAndReportsTruncation) {
  const auto logs_directory = TempPath("azookey-diag-bounded-runtime-logs");
  std::error_code ec;
  std::filesystem::remove_all(logs_directory, ec);
  ASSERT_TRUE(std::filesystem::create_directories(logs_directory));
  {
    std::ofstream output(logs_directory / "tip-20260802.jsonl.1");
    for (int index = 0; index < 30000; ++index) {
      output << R"({"event":"bulk","candidate":"private body","index":)" << index << "}\n";
    }
  }

  diag::ProbeResult result;
  result.logs_directory = logs_directory;
  const auto entries = diag::BuildCollectionEntries(result);
  const auto log = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
    return entry.name == "logs/tip-20260802.jsonl.1";
  });
  const auto note = std::find_if(entries.begin(), entries.end(),
                                 [](const auto& entry) { return entry.name == "logs/README.txt"; });
  ASSERT_NE(log, entries.end());
  ASSERT_NE(note, entries.end());
  EXPECT_LE(log->content.size(), 1024U * 1024U);
  EXPECT_EQ(log->content.find("private body"), std::string::npos);
  EXPECT_NE(log->content.find(R"("index":29999)"), std::string::npos);
  EXPECT_NE(note->content.find("Truncated files: 1"), std::string::npos);

  std::filesystem::remove_all(logs_directory, ec);
}

TEST(DiagnosticsTest, CollectionBoundsTotalRuntimeLogBytes) {
  const auto logs_directory = TempPath("azookey-diag-total-bounded-runtime-logs");
  std::error_code ec;
  std::filesystem::remove_all(logs_directory, ec);
  ASSERT_TRUE(std::filesystem::create_directories(logs_directory));
  const std::string padding(220, 'x');
  for (int file_index = 1; file_index <= 9; ++file_index) {
    const auto path = logs_directory / ("tip-2026080" + std::to_string(file_index) + ".jsonl.1");
    std::ofstream output(path);
    for (int line_index = 0; line_index < 6000; ++line_index) {
      output << R"({"event":"bulk","detail":")" << padding << R"(","index":)" << line_index
             << "}\n";
    }
  }

  diag::ProbeResult result;
  result.logs_directory = logs_directory;
  const auto entries = diag::BuildCollectionEntries(result);
  uintmax_t collected_log_bytes = 0;
  const diag::ArchiveEntry* note = nullptr;
  for (const auto& entry : entries) {
    if (entry.name.rfind("logs/tip-", 0) == 0) collected_log_bytes += entry.content.size();
    if (entry.name == "logs/README.txt") note = &entry;
  }
  EXPECT_LE(collected_log_bytes, 8U * 1024U * 1024U);
  ASSERT_NE(note, nullptr);
  EXPECT_NE(note->content.find("8388608 bytes total"), std::string::npos);

  std::filesystem::remove_all(logs_directory, ec);
}

TEST(DiagnosticsTest, LoadedModelMustMatchSelectedModelPath) {
  diag::Snapshot snapshot;
  snapshot.model_enabled = true;
  snapshot.selected_model_path = R"(C:\Models\selected.gguf)";
  snapshot.selected_model_exists = true;
  snapshot.selected_model_valid = true;
  azookey::ipc::QueryDiagnosticsPayload host;
  host.model_loaded = true;
  host.loaded_model_path = R"(C:\Models\other.gguf)";
  host.engine = "llama_cpp";
  host.backend = "cpu";
  host.fallback_state = "healthy";
  snapshot.host_diagnostics = host;

  const auto report = diag::EvaluateSnapshot(snapshot, 1);
  const auto model_check = std::find_if(report.checks.begin(), report.checks.end(),
                                        [](const auto& check) { return check.id == "D-008"; });
  ASSERT_NE(model_check, report.checks.end());
  EXPECT_EQ(model_check->status, diag::Status::Warning);
  const auto details = j::Parse(model_check->details_json);
  ASSERT_TRUE(details.has_value());
  EXPECT_FALSE(details->GetBool("loaded").value_or(true));
}

TEST(DiagnosticsTest, StoredZipContainsOnlyDeclaredMembers) {
  const auto zip_path = TempPath("azookey-diag-test.zip");
  std::string error;
  ASSERT_TRUE(
      diag::WriteZip(zip_path, {{"diag.json", "{}"}, {"environment.txt", "safe\n"}}, &error))
      << error;
  std::ifstream input(zip_path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  ASSERT_GE(bytes.size(), 4U);
  EXPECT_EQ(bytes.substr(0, 4), std::string("PK\x03\x04", 4));
  ASSERT_GE(bytes.size(), 14U);
  const auto dos_date = static_cast<uint16_t>(static_cast<unsigned char>(bytes[12])) |
                        (static_cast<uint16_t>(static_cast<unsigned char>(bytes[13])) << 8);
  EXPECT_NE(dos_date, 0);
  EXPECT_NE(bytes.find("diag.json"), std::string::npos);
  EXPECT_NE(bytes.find("environment.txt"), std::string::npos);
  EXPECT_NE(bytes.find("safe\n"), std::string::npos);

  std::error_code ec;
  std::filesystem::remove(zip_path, ec);
}

TEST(DiagnosticsTest, FailedZipWriteRemovesTruncatedOutput) {
  const auto zip_path = TempPath("azookey-diag-invalid.zip");
  {
    std::ofstream output(zip_path, std::ios::binary);
    output << "previous";
  }
  std::string error;
  EXPECT_FALSE(diag::WriteZip(zip_path, {{"../invalid", "data"}}, &error));
  EXPECT_FALSE(std::filesystem::exists(zip_path));
}

TEST(DiagnosticsTest, ProbesFilesWrittenByRuntimeStores) {
  const auto learning_path = TempPath("azookey-diag-learning.tsv");
  const auto dictionary_path = TempPath("azookey-diag-user-dictionary.json");
  std::error_code ec;
  std::filesystem::remove(learning_path, ec);
  std::filesystem::remove(dictionary_path, ec);

  azookey::learning::LearningStore learning(learning_path.string());
  learning.Observe("よみ", "表記", 0.8, 100);
  ASSERT_TRUE(learning.Save());
  uint64_t learning_entries = 0;
  EXPECT_TRUE(diag::ProbeLearningStoreFile(learning_path, &learning_entries));
  EXPECT_EQ(learning_entries, 1);

  azookey::learning::UserDictionary dictionary(dictionary_path.string());
  EXPECT_TRUE(dictionary.Add({"単語", "たんご", std::nullopt, std::nullopt, std::nullopt}));
  ASSERT_TRUE(dictionary.Save());
  uint64_t dictionary_entries = 0;
  uint64_t skipped_entries = 0;
  EXPECT_TRUE(
      diag::ProbeUserDictionaryFile(dictionary_path, &dictionary_entries, &skipped_entries));
  EXPECT_EQ(dictionary_entries, 1);
  EXPECT_EQ(skipped_entries, 0);

  std::filesystem::remove(learning_path, ec);
  std::filesystem::remove(dictionary_path, ec);
}

TEST(DiagnosticsTest, UserDictionaryProbeMatchesRuntimeInvalidEntryTolerance) {
  const auto dictionary_path = TempPath("azookey-diag-user-dictionary-skipped.json");
  {
    std::ofstream output(dictionary_path);
    output << R"({"version":1,"entries":[{"word":"単語","ruby":"たんご"},{"word":42}]})";
  }

  uint64_t entries = 0;
  uint64_t skipped_entries = 0;
  EXPECT_TRUE(diag::ProbeUserDictionaryFile(dictionary_path, &entries, &skipped_entries));
  EXPECT_EQ(entries, 1);
  EXPECT_EQ(skipped_entries, 1);

  std::error_code ec;
  std::filesystem::remove(dictionary_path, ec);
}

TEST(DiagnosticsTest, LogsProbeUsesRuntimeLoggerPathAndRepairCreatesDirectory) {
  EXPECT_EQ(diag::RuntimeLogsDirectory(),
            azookey::logging::RuntimeLoggerOptionsFromEnvironment("diagnostics").logs_directory);

  const auto logs_directory = TempPath("azookey-diag-repair-logs");
  std::error_code ec;
  std::filesystem::remove_all(logs_directory, ec);
  EXPECT_FALSE(diag::ProbeLogsDirectory(logs_directory));
  std::string error;
  EXPECT_TRUE(diag::RepairLogsDirectory(logs_directory, &error)) << error;
  const auto stale_probe = logs_directory / ".azookey-diag-write-probe-stale.tmp";
  const auto fresh_probe = logs_directory / ".azookey-diag-write-probe-fresh.tmp";
  std::ofstream(stale_probe) << "stale";
  std::ofstream(fresh_probe) << "fresh";
  std::filesystem::last_write_time(
      stale_probe, std::filesystem::file_time_type::clock::now() - std::chrono::hours(48));
  EXPECT_TRUE(diag::ProbeLogsDirectory(logs_directory));
  EXPECT_FALSE(std::filesystem::exists(stale_probe));
  EXPECT_TRUE(std::filesystem::exists(fresh_probe));
  std::filesystem::remove_all(logs_directory, ec);
}

TEST(DiagnosticsTest, RepairRunsRegistrationOnceAndRechecksEveryRepairableItem) {
  diag::Snapshot before_snapshot;
  diag::Snapshot after_snapshot;
  after_snapshot.tip_path_registered = true;
  after_snapshot.tip_path_exists = true;
  after_snapshot.tip_bitness_matches = true;
  after_snapshot.com_registration_matches = true;
  after_snapshot.language_profile_registered = true;
  after_snapshot.logs_directory_writable = true;

  diag::ProbeResult before;
  before.report = diag::EvaluateSnapshot(before_snapshot, 1);
  before.logs_directory = TempPath("azookey-diag-hook-logs");
  diag::ProbeResult after;
  after.report = diag::EvaluateSnapshot(after_snapshot, 2);
  after.logs_directory = before.logs_directory;

  int probes = 0;
  int registration_repairs = 0;
  int logs_repairs = 0;
  diag::RepairHooks hooks;
  hooks.probe_system = [&] { return probes++ == 0 ? before : after; };
  hooks.repair_registration = [&](const diag::ProbeResult&) {
    ++registration_repairs;
    return diag::RepairOperationResult{diag::RepairStatus::Succeeded, "registration refreshed"};
  };
  hooks.repair_logs_directory = [&](const std::filesystem::path&) {
    ++logs_repairs;
    return diag::RepairOperationResult{diag::RepairStatus::Succeeded, "logs created"};
  };

  const auto repair = diag::RepairSystem(hooks);
  EXPECT_EQ(probes, 2);
  EXPECT_EQ(registration_repairs, 1);
  EXPECT_EQ(logs_repairs, 1);
  ASSERT_EQ(repair.repairs.size(), 4U);
  EXPECT_TRUE(std::all_of(repair.repairs.begin(), repair.repairs.end(), [](const auto& item) {
    return item.status == diag::RepairStatus::Succeeded &&
           item.before_status == diag::Status::Error && item.after_status == diag::Status::Ok;
  }));
  EXPECT_TRUE(diag::RepairReportSucceeded(repair));
  const auto json = j::Parse(diag::SerializeRepairReport(repair));
  ASSERT_TRUE(json.has_value());
  EXPECT_EQ(json->GetBool("probe_failed"), false);
  ASSERT_NE(json->GetArray("checks"), nullptr);
  EXPECT_TRUE(json->GetString("status").has_value());
  ASSERT_NE(json->GetArray("repairs"), nullptr);
  EXPECT_EQ(json->GetArray("repairs")->size(), 4U);
}

TEST(DiagnosticsTest, RepairIsIdempotentAndReportsPermissionDenial) {
  diag::Snapshot healthy_snapshot;
  healthy_snapshot.tip_path_registered = true;
  healthy_snapshot.tip_path_exists = true;
  healthy_snapshot.tip_bitness_matches = true;
  healthy_snapshot.com_registration_matches = true;
  healthy_snapshot.language_profile_registered = true;
  healthy_snapshot.logs_directory_writable = true;
  diag::ProbeResult healthy;
  healthy.report = diag::EvaluateSnapshot(healthy_snapshot, 1);

  bool repair_called = false;
  diag::RepairHooks idempotent_hooks;
  idempotent_hooks.probe_system = [&] { return healthy; };
  idempotent_hooks.repair_registration = [&](const diag::ProbeResult&) {
    repair_called = true;
    return diag::RepairOperationResult{};
  };
  idempotent_hooks.repair_logs_directory = [&](const std::filesystem::path&) {
    repair_called = true;
    return diag::RepairOperationResult{};
  };
  const auto idempotent = diag::RepairSystem(idempotent_hooks);
  EXPECT_FALSE(repair_called);
  EXPECT_TRUE(diag::RepairReportSucceeded(idempotent));
  EXPECT_TRUE(
      std::all_of(idempotent.repairs.begin(), idempotent.repairs.end(),
                  [](const auto& item) { return item.status == diag::RepairStatus::NotNeeded; }));

  diag::Snapshot broken_registration = healthy_snapshot;
  broken_registration.tip_path_registered = false;
  broken_registration.tip_path_exists = false;
  broken_registration.tip_bitness_matches = false;
  broken_registration.com_registration_matches = false;
  broken_registration.language_profile_registered = false;
  diag::ProbeResult broken;
  broken.report = diag::EvaluateSnapshot(broken_registration, 1);
  int probes = 0;
  diag::RepairHooks denied_hooks;
  denied_hooks.probe_system = [&] {
    ++probes;
    return broken;
  };
  denied_hooks.repair_registration = [](const diag::ProbeResult&) {
    return diag::RepairOperationResult{diag::RepairStatus::PermissionDenied, "elevation required"};
  };
  denied_hooks.repair_logs_directory = [](const std::filesystem::path&) {
    return diag::RepairOperationResult{};
  };
  const auto denied = diag::RepairSystem(denied_hooks);
  EXPECT_EQ(probes, 2);
  EXPECT_FALSE(diag::RepairReportSucceeded(denied));
  EXPECT_EQ(denied.repairs[0].status, diag::RepairStatus::PermissionDenied);
  EXPECT_EQ(denied.repairs[1].status, diag::RepairStatus::PermissionDenied);
  EXPECT_EQ(denied.repairs[2].status, diag::RepairStatus::PermissionDenied);
  EXPECT_EQ(denied.repairs[3].status, diag::RepairStatus::NotNeeded);
}

TEST(DiagnosticsTest, RepairReportsPostProbeFailureWithoutClaimingSuccess) {
  diag::Snapshot healthy_snapshot;
  healthy_snapshot.tip_path_registered = true;
  healthy_snapshot.tip_path_exists = true;
  healthy_snapshot.tip_bitness_matches = true;
  healthy_snapshot.com_registration_matches = true;
  healthy_snapshot.language_profile_registered = true;
  healthy_snapshot.logs_directory_writable = true;
  diag::ProbeResult healthy;
  healthy.report = diag::EvaluateSnapshot(healthy_snapshot, 1);

  int probes = 0;
  diag::RepairHooks hooks;
  hooks.probe_system = [&] {
    if (probes++ == 0) return healthy;
    throw std::runtime_error("post-probe failure");
  };
  const auto repair = diag::RepairSystem(hooks);
  EXPECT_TRUE(repair.probe_failed);
  EXPECT_FALSE(diag::RepairReportSucceeded(repair));
  EXPECT_TRUE(std::all_of(repair.repairs.begin(), repair.repairs.end(), [](const auto& item) {
    return item.status == diag::RepairStatus::NotNeeded && !item.after_status.has_value();
  }));
  const auto json = j::Parse(diag::SerializeRepairReport(repair));
  ASSERT_TRUE(json.has_value());
  EXPECT_EQ(json->GetBool("probe_failed"), true);
  const auto* repairs = json->GetArray("repairs");
  ASSERT_NE(repairs, nullptr);
  ASSERT_FALSE(repairs->empty());
  const auto* after_status = repairs->front().Find("after_status");
  ASSERT_NE(after_status, nullptr);
  EXPECT_TRUE(after_status->IsNull());
}

TEST(DiagnosticsTest, RepairReportsRegistrationRegressionAfterRollback) {
  diag::Snapshot before_snapshot;
  before_snapshot.tip_path_registered = true;
  before_snapshot.tip_path_exists = true;
  before_snapshot.tip_bitness_matches = true;
  before_snapshot.language_profile_registered = true;
  before_snapshot.logs_directory_writable = true;
  diag::ProbeResult before;
  before.report = diag::EvaluateSnapshot(before_snapshot, 1);

  diag::Snapshot after_snapshot;
  after_snapshot.logs_directory_writable = true;
  diag::ProbeResult after;
  after.report = diag::EvaluateSnapshot(after_snapshot, 2);

  int probes = 0;
  diag::RepairHooks hooks;
  hooks.probe_system = [&] { return probes++ == 0 ? before : after; };
  hooks.repair_registration = [](const diag::ProbeResult&) {
    return diag::RepairOperationResult{diag::RepairStatus::Failed, "regsvr32 failed"};
  };
  const auto repair = diag::RepairSystem(hooks);
  ASSERT_EQ(repair.repairs.size(), 4U);
  EXPECT_EQ(repair.repairs[0].status, diag::RepairStatus::Failed);
  EXPECT_EQ(repair.repairs[0].before_status, diag::Status::Ok);
  EXPECT_EQ(repair.repairs[0].after_status, diag::Status::Error);
  EXPECT_NE(repair.repairs[0].message.find("regressed"), std::string::npos);
  EXPECT_EQ(repair.repairs[1].status, diag::RepairStatus::Failed);
  EXPECT_EQ(repair.repairs[2].status, diag::RepairStatus::Failed);
  EXPECT_NE(repair.repairs[2].message.find("regressed"), std::string::npos);
  EXPECT_EQ(repair.repairs[3].status, diag::RepairStatus::NotNeeded);
}

TEST(DiagnosticsTest, EmbeddedSchemaAndDefaultSettingsStaySupported) {
  const auto sample =
      std::filesystem::path(AZOOKEY_TEST_SOURCE_DIR) / "settings/default-settings.sample.json";
  EXPECT_TRUE(diag::EmbeddedSettingsSchemaUsesOnlySupportedKeywords());
  EXPECT_TRUE(diag::ProbeSettingsFile(sample));
}

}  // namespace
