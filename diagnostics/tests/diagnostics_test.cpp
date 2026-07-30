#include "Diagnostics.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "azookey/ipc/Json.h"

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
      "details": {"entries": 3}
    },
    {
      "id": "D-012",
      "name": "settings",
      "status": "ok",
      "message": "Settings conform to the embedded schema",
      "details": {"missing": false, "valid": true}
    }
  ]
}
)json");
  ASSERT_TRUE(expected.has_value());
  EXPECT_EQ(actual, j::Stringify(*expected));
}

TEST(DiagnosticsTest, CollectionSnapshotExcludesSensitiveBodies) {
  const auto settings_path = TempPath("azookey-diag-settings-redaction.json");
  {
    std::ofstream output(settings_path);
    output << R"({"openAiApiKey":"sk-secret","promptPrefixByApp":{"app":"private prompt"},)"
              R"("model":{"enabled":true,"selectedPath":"C:\\Users\\alice\\model.gguf"}})";
  }
  diag::ProbeResult result;
  result.settings_path = settings_path;
  result.report.timestamp_ms = 1;
  result.report.checks.push_back(
      {"D-001", "tip_dll", diag::Status::Ok, "ok", R"({"token":"sk-diagnostic"})"});
  result.host_health_json =
      R"({"last_error":"Bearer private-token at C:\\Users\\alice\\model.gguf"})";
  result.ipc_ping_json = R"({"status":"ok","rtt_ms":1})";

  const auto entries = diag::BuildCollectionEntries(result);
  const std::vector<std::string> expected_names = {
      "diag.json",     "settings.redacted.json", "host-health.json",
      "ipc-ping.json", "environment.txt",        "crash-summary.txt",
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
  EXPECT_EQ(combined.find("alice"), std::string::npos);
  EXPECT_NE(combined.find("***redacted***"), std::string::npos);
  EXPECT_NE(combined.find("%USERPROFILE%"), std::string::npos);

  std::error_code ec;
  std::filesystem::remove(settings_path, ec);
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
  EXPECT_NE(bytes.find("diag.json"), std::string::npos);
  EXPECT_NE(bytes.find("environment.txt"), std::string::npos);
  EXPECT_NE(bytes.find("safe\n"), std::string::npos);

  std::error_code ec;
  std::filesystem::remove(zip_path, ec);
}

TEST(DiagnosticsTest, QueryDiagnosticsPayloadRoundTrips) {
  azookey::ipc::QueryDiagnosticsPayload payload;
  payload.model_loaded = true;
  payload.loaded_model_path = R"(C:\Models\model.gguf)";
  payload.engine = "llama_cpp";
  payload.backend = "cuda";
  payload.rss_mb = 256;
  payload.learning_entries = 10;
  payload.user_dict_entries = 4;
  payload.fallback_state = "healthy";
  const auto parsed =
      azookey::ipc::ParseQueryDiagnostics(azookey::ipc::BuildQueryDiagnostics(payload));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->model_loaded);
  EXPECT_EQ(parsed->loaded_model_path, payload.loaded_model_path);
  EXPECT_EQ(parsed->engine, "llama_cpp");
  EXPECT_EQ(parsed->fallback_state, "healthy");
}

}  // namespace
