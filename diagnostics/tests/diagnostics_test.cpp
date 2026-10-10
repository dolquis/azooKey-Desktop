#include "Diagnostics.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "azookey/ipc/HandshakeToken.h"
#include "azookey/ipc/Json.h"
#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/UserDictionary.h"
#include "azookey/logging/RuntimeLogger.h"

namespace {

namespace diag = ::azookey::diagnostics;
namespace j = ::azookey::ipc::json;

std::filesystem::path TempPath(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

std::filesystem::path UniqueTempDirectory(const char* prefix) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto path = std::filesystem::temp_directory_path() /
                      (std::string(prefix) + std::to_string(std::random_device{}()));
    if (std::filesystem::create_directory(path)) return path;
  }
  throw std::runtime_error("unable to create unique diagnostic test directory");
}

class MockDpapiCrypto final : public azookey::learning::ByteCrypto {
 public:
  bool available{true};
  bool decrypt_succeeds{true};
  mutable int decrypt_calls{};

  bool IsAvailable() const override { return available; }
  bool Encrypt(const std::vector<uint8_t>&, std::vector<uint8_t>&) const override {
    return false;
  }
  bool Decrypt(const std::vector<uint8_t>& cipher,
               std::vector<uint8_t>& plain) const override {
    ++decrypt_calls;
    if (cipher != std::vector<uint8_t>({1, 2, 3}) || !decrypt_succeeds) return false;
    plain.assign({'s', 'e', 'c', 'r', 'e', 't', '-', 'b', 'o', 'd', 'y'});
    return true;
  }
};

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
  snapshot.foreground_app = {true, true, diag::AppProcessArchitecture::X64, false};
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
      "details": {"matches": true, "optional_missing": false}
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
    },
    {
      "id": "D-014",
      "name": "dpapi",
      "status": "ok",
      "message": "No effective OpenAI backend requires an API key",
      "details": {"state": "not_required"}
    },
    {
      "id": "D-015",
      "name": "app_compatibility",
      "status": "warning",
      "message": "Foreground app TSF context has not been verified",
      "details": {"reason": "context_unverified"}
    }
  ]
}
)json");
  ASSERT_TRUE(expected.has_value());
  EXPECT_EQ(actual, j::Stringify(*expected));
}

TEST(DiagnosticsTest, DpapiRequiresKeyOnlyForEffectiveOpenAiBackends) {
  MockDpapiCrypto crypto;
  const auto probe = [&](std::string_view json) {
    return diag::ProbeDpapiSettingsJson(json, crypto);
  };
  EXPECT_EQ(probe(R"({"aiBackend":"none"})"), diag::DpapiState::NotRequired);
  EXPECT_EQ(probe(R"({"aiBackend":"none","openAiApiKey":"dpapi:AQID"})"),
            diag::DpapiState::NotRequired);
  EXPECT_EQ(probe(R"({"aiBackend":"openai"})"), diag::DpapiState::MissingKey);
  EXPECT_EQ(probe(R"({"aiBackend":"openai","openAiApiKey":"legacy-key"})"),
            diag::DpapiState::Plaintext);
  EXPECT_EQ(probe(R"({"aiBackend":"none","profilesByApp":{"app.exe":{"aiBackend":"openai"}}})"),
            diag::DpapiState::MissingKey);
  EXPECT_EQ(probe(R"({"aiBackend":"none","profilesByApp":{"default":{"aiBackend":"openai"}}})"),
            diag::DpapiState::MissingKey);
  EXPECT_EQ(probe(R"({"aiBackend":"none","profilesByApp":{"default":{"aiBackend":"openai","privacyMode":"private"},"app.exe":{"privacyMode":"normal"}}})"),
            diag::DpapiState::MissingKey);
  EXPECT_EQ(probe(R"({"aiBackend":"none","profilesByApp":{"app.exe":{"aiBackend":"openai","privacyMode":"private"}}})"),
            diag::DpapiState::NotRequired);
  EXPECT_EQ(probe(R"({"aiBackend":"none","profilesByApp":{"app.exe":{"aiBackend":"openai","privacyMode":"secure"}}})"),
            diag::DpapiState::NotRequired);
  EXPECT_EQ(probe(R"({"aiBackend":"openai","openAiApiKey":"dpapi:AQID","privacy":{"mode":"offline"}})"),
            diag::DpapiState::NotRequired);
  EXPECT_EQ(probe(R"({"aiBackend":"none","privacy":{"mode":"offline"},"profilesByApp":{"app.exe":{"aiBackend":"openai","privacyMode":"normal"}}})"),
            diag::DpapiState::NotRequired);
  EXPECT_EQ(probe(R"({"aiBackend":"openai","privacy":{"mode":"custom","custom":{"externalAi":false}}})"),
            diag::DpapiState::NotRequired);
  EXPECT_EQ(crypto.decrypt_calls, 0);
}

TEST(DiagnosticsTest, DpapiProbeDistinguishesDecryptionFailureAndUnavailableWithoutLeakingKey) {
  MockDpapiCrypto crypto;
  constexpr std::string_view settings =
      R"({"aiBackend":"openai","openAiApiKey":"dpapi:AQID"})";
  EXPECT_EQ(diag::ProbeDpapiSettingsJson(settings, crypto), diag::DpapiState::Decrypted);
  crypto.decrypt_succeeds = false;
  EXPECT_EQ(diag::ProbeDpapiSettingsJson(settings, crypto), diag::DpapiState::DecryptFailed);
  crypto.available = false;
  EXPECT_EQ(diag::ProbeDpapiSettingsJson(settings, crypto), diag::DpapiState::Unavailable);
  EXPECT_EQ(crypto.decrypt_calls, 2);
  EXPECT_EQ(diag::ProbeDpapiSettingsJson(
                R"({"aiBackend":"openai","openAiApiKey":"dpapi:invalid"})", crypto),
            diag::DpapiState::DecryptFailed);
  EXPECT_EQ(diag::ProbeDpapiSettingsJson("{", crypto), diag::DpapiState::Unavailable);

  diag::Snapshot snapshot;
  snapshot.dpapi_state = diag::DpapiState::Decrypted;
  const auto success_report = diag::EvaluateSnapshot(snapshot, 1);
  const auto dpapi_check = [](const diag::Report& report) -> const diag::Check& {
    return *std::find_if(report.checks.begin(), report.checks.end(),
                         [](const diag::Check& check) { return check.id == "D-014"; });
  };
  EXPECT_EQ(dpapi_check(success_report).status, diag::Status::Ok);
  const auto success = diag::SerializeReport(success_report);
  EXPECT_EQ(success.find("secret-body"), std::string::npos);
  EXPECT_NE(success.find(R"("state":"decrypted")"), std::string::npos);
  snapshot.dpapi_state = diag::DpapiState::MissingKey;
  EXPECT_EQ(dpapi_check(diag::EvaluateSnapshot(snapshot, 1)).status, diag::Status::Warning);
  snapshot.dpapi_state = diag::DpapiState::DecryptFailed;
  const auto failure_report = diag::EvaluateSnapshot(snapshot, 1);
  EXPECT_EQ(dpapi_check(failure_report).status, diag::Status::Error);
  const auto failure = diag::SerializeReport(failure_report);
  EXPECT_NE(failure.find(R"("state":"decrypt_failed")"), std::string::npos);
  snapshot.dpapi_state = diag::DpapiState::Unavailable;
  const auto unavailable_report = diag::EvaluateSnapshot(snapshot, 1);
  EXPECT_EQ(dpapi_check(unavailable_report).status, diag::Status::Warning);
  const auto unavailable = diag::SerializeReport(unavailable_report);
  EXPECT_NE(unavailable.find(R"("state":"unavailable")"), std::string::npos);
}

TEST(DiagnosticsTest, AppCompatibilityClassifiesKnownExclusionsAndUnknownContext) {
  diag::ForegroundAppInfo info;
  const auto reason = [&] { return diag::ClassifyAppCompatibility(info); };
  EXPECT_EQ(reason(), diag::AppCompatibilityReason::NoForegroundWindow);
  info.has_window = true;
  EXPECT_EQ(reason(), diag::AppCompatibilityReason::ProcessUnavailable);
  info.process_opened = true;
  EXPECT_EQ(reason(), diag::AppCompatibilityReason::ArchitectureUnknown);
  info.architecture = diag::AppProcessArchitecture::X86;
  EXPECT_EQ(reason(), diag::AppCompatibilityReason::X86);
  info.architecture = diag::AppProcessArchitecture::Other;
  EXPECT_EQ(reason(), diag::AppCompatibilityReason::UnsupportedArchitecture);
  info.architecture = diag::AppProcessArchitecture::X64;
  EXPECT_EQ(reason(), diag::AppCompatibilityReason::ContainerUnknown);
  info.app_container = true;
  EXPECT_EQ(reason(), diag::AppCompatibilityReason::AppContainer);
  info.app_container = false;
  EXPECT_EQ(reason(), diag::AppCompatibilityReason::ContextUnverified);
}

TEST(DiagnosticsTest, AppCompatibilityDistinguishesX64EmulationFromArm64Native) {
  constexpr auto unknown = diag::kProcessMachineUnknown;
  constexpr auto arm64 = diag::kProcessMachineArm64;
  const auto classify = [](uint16_t process, uint16_t native, std::optional<uint16_t> resolved) {
    return diag::ClassifyAppProcessArchitecture(process, native, resolved);
  };
  EXPECT_EQ(classify(diag::kProcessMachineX86, arm64, std::nullopt),
            diag::AppProcessArchitecture::X86);
  EXPECT_EQ(classify(unknown, diag::kProcessMachineX64, std::nullopt),
            diag::AppProcessArchitecture::X64);
  EXPECT_EQ(classify(unknown, arm64, diag::kProcessMachineX64), diag::AppProcessArchitecture::X64);
  EXPECT_EQ(classify(unknown, arm64, arm64), diag::AppProcessArchitecture::Other);
  EXPECT_EQ(classify(unknown, arm64, std::nullopt), diag::AppProcessArchitecture::Unknown);

  diag::ForegroundAppInfo info{true, true, classify(unknown, arm64, diag::kProcessMachineX64),
                               false};
  EXPECT_EQ(diag::ClassifyAppCompatibility(info), diag::AppCompatibilityReason::ContextUnverified);
  info.architecture = classify(unknown, arm64, arm64);
  EXPECT_EQ(diag::ClassifyAppCompatibility(info),
            diag::AppCompatibilityReason::UnsupportedArchitecture);
  info.architecture = classify(unknown, arm64, std::nullopt);
  EXPECT_EQ(diag::ClassifyAppCompatibility(info),
            diag::AppCompatibilityReason::ArchitectureUnknown);

  diag::Snapshot snapshot;
  snapshot.foreground_app = info;
  const auto d015 = [&] { return diag::EvaluateSnapshot(snapshot, 1).checks.back(); };
  EXPECT_EQ(d015().status, diag::Status::Warning);
  snapshot.foreground_app.architecture = classify(unknown, arm64, diag::kProcessMachineX64);
  EXPECT_EQ(d015().status, diag::Status::Warning);
  snapshot.foreground_app.architecture = classify(unknown, arm64, arm64);
  EXPECT_EQ(d015().status, diag::Status::Error);
}

TEST(DiagnosticsTest, AppCompatibilityJsonUsesStableReasonWithoutWindowText) {
  diag::Snapshot snapshot;
  snapshot.foreground_app = {true, true, diag::AppProcessArchitecture::X86, false};
  const auto report = diag::EvaluateSnapshot(snapshot, 1);
  const auto& check = report.checks.back();
  EXPECT_EQ(check.id, "D-015");
  EXPECT_EQ(check.status, diag::Status::Error);
  EXPECT_EQ(diag::AppCompatibilityReasonName(diag::ClassifyAppCompatibility(snapshot.foreground_app)),
            "x86");
  const auto json = diag::SerializeReport(report);
  EXPECT_NE(json.find(R"("reason":"x86")"), std::string::npos);
  EXPECT_EQ(json.find("window_title"), std::string::npos);
  EXPECT_EQ(json.find("process_name"), std::string::npos);

  snapshot.foreground_app = {true, true, diag::AppProcessArchitecture::X64, true};
  const auto container_report = diag::EvaluateSnapshot(snapshot, 1);
  EXPECT_EQ(container_report.checks.back().status, diag::Status::Error);
  EXPECT_NE(diag::SerializeReport(container_report).find(R"("reason":"app_container")"),
            std::string::npos);

  snapshot.foreground_app.app_container = false;
  const auto unverified_report = diag::EvaluateSnapshot(snapshot, 1);
  EXPECT_EQ(unverified_report.checks.back().status, diag::Status::Warning);
  EXPECT_NE(diag::SerializeReport(unverified_report).find(R"("reason":"context_unverified")"),
            std::string::npos);
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

TEST(DiagnosticsTest, CollectionNeverIncludesPublishedHandshakeToken) {
  const auto config_directory = TempPath("azookey-diag-handshake-token-config");
  const auto logs_directory = config_directory / "logs";
  std::error_code ec;
  std::filesystem::remove_all(config_directory, ec);
  ASSERT_TRUE(std::filesystem::create_directories(logs_directory));
  const auto token = azookey::ipc::GenerateHandshakeToken();
  ASSERT_TRUE(token);
  ASSERT_TRUE(azookey::ipc::PublishHandshakeToken(config_directory / "ipc-token", *token));
  const auto settings_path = config_directory / "settings.json";
  {
    std::ofstream output(settings_path);
    output << R"({"maxCandidates":9})";
  }
  {
    std::ofstream output(logs_directory / "host-20260802.jsonl");
    output << R"({"event":"query_latency","result":"ok"})" << '\n';
  }

  diag::ProbeResult result;
  result.settings_path = settings_path;
  result.logs_directory = logs_directory;
  result.report.timestamp_ms = 1;
  result.host_health_json = R"({"status":"ok"})";
  result.ipc_ping_json = R"({"status":"ok","rtt_ms":1})";
  const auto entries = diag::BuildCollectionEntries(result);
  ASSERT_FALSE(entries.empty());
  for (const auto& entry : entries) {
    EXPECT_EQ(entry.name.find("ipc-token"), std::string::npos) << entry.name;
    EXPECT_EQ(entry.content.find(*token), std::string::npos) << entry.name;
  }

  std::filesystem::remove_all(config_directory, ec);
}

TEST(DiagnosticsTest, RedactedSettingsKeepTypedControlsAndHideSensitiveValues) {
  const auto redacted = diag::RedactSettingsJson(R"json({
    "maxContextLength": 10,
    "includeContextInAITransform": true,
    "contextReselection": false,
    "maxCandidates": 9,
    "emojiMaxCandidates": 12,
    "emojiTriggerMinQueryLength": 1,
    "openAiApiKey": "sk-secret",
    "promptPrefixByApp": {"app.exe": "private prompt"},
    "profilesByApp": {"app.exe": {
      "promptPrefix": "private profile prompt",
      "candidateTagBoosts": {"technical": 1.5},
      "privacyMode": "private"
    }},
    "privacy": {"custom": {"aiCandidate": true}},
    "unknownSecret": {"nested": "private value"},
    "unknownToken": ["private array value"],
    "unexpected": {"maxCandidates": "private malformed value"}
  })json");
  const auto value = j::Parse(redacted);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(value->GetNumber("maxContextLength"), 10);
  EXPECT_EQ(value->GetBool("includeContextInAITransform"), true);
  EXPECT_EQ(value->GetBool("contextReselection"), false);
  EXPECT_EQ(value->GetNumber("maxCandidates"), 9);
  EXPECT_EQ(value->GetNumber("emojiMaxCandidates"), 12);
  EXPECT_EQ(value->GetNumber("emojiTriggerMinQueryLength"), 1);
  EXPECT_EQ(value->GetString("openAiApiKey"), "***redacted***");
  const auto* prompts = value->Find("promptPrefixByApp");
  ASSERT_NE(prompts, nullptr);
  EXPECT_EQ(prompts->GetString("app.exe"), "***redacted***");
  const auto* profiles = value->Find("profilesByApp");
  ASSERT_NE(profiles, nullptr);
  const auto* profile = profiles->Find("app.exe");
  ASSERT_NE(profile, nullptr);
  EXPECT_EQ(profile->GetString("promptPrefix"), "***redacted***");
  const auto* boosts = profile->Find("candidateTagBoosts");
  ASSERT_NE(boosts, nullptr);
  EXPECT_EQ(boosts->GetNumber("technical"), 1.5);
  EXPECT_EQ(profile->GetString("privacyMode"), "private");
  const auto* privacy = value->Find("privacy");
  ASSERT_NE(privacy, nullptr);
  const auto* custom = privacy->Find("custom");
  ASSERT_NE(custom, nullptr);
  EXPECT_EQ(custom->GetBool("aiCandidate"), true);
  EXPECT_EQ(value->GetString("unknownSecret"), "***redacted***");
  EXPECT_EQ(value->GetString("unknownToken"), "***redacted***");
  const auto* unexpected = value->Find("unexpected");
  ASSERT_NE(unexpected, nullptr);
  EXPECT_EQ(unexpected->GetString("maxCandidates"), "***redacted***");
  EXPECT_EQ(redacted.find("private prompt"), std::string::npos);
  EXPECT_EQ(redacted.find("private malformed value"), std::string::npos);
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
  bool migration_available = true;
  EXPECT_TRUE(diag::ProbeLearningStoreFile(learning_path, &learning_entries, &migration_available));
  EXPECT_EQ(learning_entries, 1);
  EXPECT_FALSE(migration_available);

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

TEST(DiagnosticsTest, LegacyLearningStoreIsWarningOnlyWhenReadable) {
  const auto directory = UniqueTempDirectory("azookey-diag-legacy-learning-");
  const auto path = directory / "learning.tsv";
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "yomi\tsurface\t0.8 100\n";
  }
  uint64_t entries = 0;
  bool migration_available = false;
  EXPECT_TRUE(diag::ProbeLearningStoreFile(path, &entries, &migration_available));
  EXPECT_EQ(entries, 1U);
  EXPECT_TRUE(migration_available);

  diag::Snapshot snapshot;
  snapshot.learning_store_migration_available = migration_available;
  auto report = diag::EvaluateSnapshot(snapshot, 1);
  const auto check = std::find_if(report.checks.begin(), report.checks.end(),
                                  [](const auto& item) { return item.id == "D-010"; });
  ASSERT_NE(check, report.checks.end());
  EXPECT_EQ(check->status, diag::Status::Warning);

  snapshot.learning_store_valid = false;
  report = diag::EvaluateSnapshot(snapshot, 1);
  const auto failed_check = std::find_if(report.checks.begin(), report.checks.end(),
                                         [](const auto& item) { return item.id == "D-010"; });
  ASSERT_NE(failed_check, report.checks.end());
  EXPECT_EQ(failed_check->status, diag::Status::Error);
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "invalid learning record\n";
  }
  EXPECT_FALSE(diag::ProbeLearningStoreFile(path, &entries, &migration_available));
  EXPECT_FALSE(migration_available);
  std::error_code ec;
  std::filesystem::remove_all(directory, ec);
}

TEST(DiagnosticsTest, OptionalComDisplayNameIsWarningOnlyWhenRequiredRegistrationMatches) {
  diag::Snapshot snapshot;
  snapshot.com_registration_matches = true;
  snapshot.com_registration_optional_missing = true;
  auto report = diag::EvaluateSnapshot(snapshot, 1);
  auto check = std::find_if(report.checks.begin(), report.checks.end(),
                            [](const auto& item) { return item.id == "D-002"; });
  ASSERT_NE(check, report.checks.end());
  EXPECT_EQ(check->status, diag::Status::Warning);

  snapshot.com_registration_matches = false;
  report = diag::EvaluateSnapshot(snapshot, 1);
  check = std::find_if(report.checks.begin(), report.checks.end(),
                       [](const auto& item) { return item.id == "D-002"; });
  ASSERT_NE(check, report.checks.end());
  EXPECT_EQ(check->status, diag::Status::Error);
}

TEST(DiagnosticsTest, CrashSummaryIncludesOnlyManagedRegularDumpMetadata) {
  const auto directory = UniqueTempDirectory("azookey-diag-crashes-");
  std::error_code ec;
  const auto managed = directory / "azookey-host-20260927T120000Z-123.dmp";
  std::ofstream(managed, std::ios::binary) << "private dump contents";
  std::ofstream(directory / "other-host-20260927T120000Z-123.dmp") << "unmanaged";
  std::filesystem::create_directory(directory / "azookey-settings-20260927T120000Z-456.dmp", ec);
  const auto linked = directory / "azookey-settings-20260927T120001Z-456.dmp";
  std::filesystem::create_symlink(managed, linked, ec);

  diag::ProbeResult result;
  result.crashes_directory = directory;
  const auto entries = diag::BuildCollectionEntries(result);
  const auto summary = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
    return entry.name == "crash-summary.txt";
  });
  ASSERT_NE(summary, entries.end());
  EXPECT_NE(summary->content.find("Managed crash dumps: 1"), std::string::npos);
  EXPECT_NE(summary->content.find("Total size (bytes): 21"), std::string::npos);
  EXPECT_NE(summary->content.find("azookey-host-20260927T120000Z-123.dmp"), std::string::npos);
  EXPECT_NE(summary->content.find(".dmp\t21\t"), std::string::npos);
  EXPECT_NE(summary->content.find("Z\n"), std::string::npos);
  EXPECT_EQ(summary->content.find("private dump contents"), std::string::npos);
  EXPECT_EQ(summary->content.find("other-host"), std::string::npos);
  EXPECT_EQ(summary->content.find("azookey-settings"), std::string::npos);
  EXPECT_EQ(summary->content.find(directory.string()), std::string::npos);

  std::filesystem::remove_all(directory, ec);
}

TEST(DiagnosticsTest, CrashSummaryDistinguishesMissingDirectoryAndNoManagedDumps) {
  const auto parent = UniqueTempDirectory("azookey-diag-empty-crashes-");
  const auto directory = parent / "crashes";
  std::error_code ec;
  diag::ProbeResult result;
  result.crashes_directory = directory;
  auto entries = diag::BuildCollectionEntries(result);
  EXPECT_EQ(entries.back().content, "No azooKey crash dump directory was found.\n");

  ASSERT_TRUE(std::filesystem::create_directories(directory));
  entries = diag::BuildCollectionEntries(result);
  EXPECT_EQ(entries.back().content, "No azooKey managed crash dumps were found.\n");
  std::filesystem::remove_all(parent, ec);
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

TEST(DiagnosticsTest, RepairSkipsOptionalComDisplayNameWarning) {
  diag::Snapshot snapshot;
  snapshot.tip_path_registered = true;
  snapshot.tip_path_exists = true;
  snapshot.tip_bitness_matches = true;
  snapshot.com_registration_matches = true;
  snapshot.com_registration_optional_missing = true;
  snapshot.language_profile_registered = true;
  snapshot.logs_directory_writable = true;
  diag::ProbeResult probe;
  probe.report = diag::EvaluateSnapshot(snapshot, 1);

  int registration_repairs = 0;
  diag::RepairHooks hooks;
  hooks.probe_system = [&] { return probe; };
  hooks.repair_registration = [&](const diag::ProbeResult&) {
    ++registration_repairs;
    return diag::RepairOperationResult{diag::RepairStatus::PermissionDenied, "elevation required"};
  };
  const auto repair = diag::RepairSystem(hooks);
  EXPECT_EQ(registration_repairs, 0);
  ASSERT_EQ(repair.repairs.size(), 4U);
  EXPECT_EQ(repair.repairs[1].before_status, diag::Status::Warning);
  EXPECT_EQ(repair.repairs[1].after_status, diag::Status::Warning);
  EXPECT_EQ(repair.repairs[1].status, diag::RepairStatus::NotNeeded);
  EXPECT_TRUE(diag::RepairReportSucceeded(repair));
}

TEST(DiagnosticsTest, RepairClearsRequiredComErrorWhenOnlyOptionalWarningRemains) {
  diag::Snapshot snapshot;
  snapshot.tip_path_registered = true;
  snapshot.tip_path_exists = true;
  snapshot.tip_bitness_matches = true;
  snapshot.language_profile_registered = true;
  snapshot.logs_directory_writable = true;
  diag::Snapshot repaired_snapshot = snapshot;
  repaired_snapshot.com_registration_matches = true;
  repaired_snapshot.com_registration_optional_missing = true;
  diag::ProbeResult before;
  before.report = diag::EvaluateSnapshot(snapshot, 1);
  diag::ProbeResult after;
  after.report = diag::EvaluateSnapshot(repaired_snapshot, 2);

  int probes = 0;
  int registration_repairs = 0;
  diag::RepairHooks hooks;
  hooks.probe_system = [&] { return probes++ == 0 ? before : after; };
  hooks.repair_registration = [&](const diag::ProbeResult&) {
    ++registration_repairs;
    return diag::RepairOperationResult{diag::RepairStatus::Succeeded, "registration refreshed"};
  };
  const auto repair = diag::RepairSystem(hooks);
  EXPECT_EQ(registration_repairs, 1);
  ASSERT_EQ(repair.repairs.size(), 4U);
  EXPECT_EQ(repair.repairs[1].before_status, diag::Status::Error);
  EXPECT_EQ(repair.repairs[1].after_status, diag::Status::Warning);
  EXPECT_EQ(repair.repairs[1].status, diag::RepairStatus::Succeeded);
  EXPECT_TRUE(diag::RepairReportSucceeded(repair));
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

TEST(DiagnosticsTest, EmbeddedSettingsSchemaValidatesCanonicalBenchmarkHistoryWithoutWriting) {
  const auto directory = UniqueTempDirectory("azookey-diag-benchmark-history-");
  const auto path = directory / "settings.json";
  const auto parsed =
      j::Parse(R"({"path":"C:/models/model.gguf","completedAt":"2026-10-10T10:00:00Z",
    "backend":"cpu","status":"success","p50_ms":1,"p95_ms":2,"p99_ms":3,"load_ms":4,
    "rss_mb":5,"vram_mb":null,"iterations_completed":6,"error":null})");
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto canonical = parsed->AsObject();
  const auto probe = [&](const j::Array& rows, bool expected) {
    j::Object model;
    model["benchmarkHistory"] = j::Value(rows);
    j::Object root;
    root["model"] = j::Value(std::move(model));
    const auto original = j::Stringify(j::Value(std::move(root)));
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << original;
    }
    EXPECT_EQ(diag::ProbeSettingsFile(path), expected) << original;
    std::ifstream in(path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), original);
  };
  probe({}, true);
  probe(j::Array(7, j::Value(canonical)), true);
  probe(j::Array(8, j::Value(canonical)), false);
  probe({j::Value(1.0)}, false);
  for (const auto* model_path :
       {"C:\\models\\model.gguf", "\\\\server\\models\\model.gguf", "/models/model.gguf"}) {
    auto row = canonical;
    row["path"] = j::Value(model_path);
    row["completedAt"] = j::Value("2024-02-29T23:59:59Z");
    row["iterations_completed"] = j::Value(4294967295.0);
    probe({j::Value(std::move(row))}, true);
  }
  for (const auto* status : {"success", "timeout", "error"}) {
    auto row = canonical;
    row["status"] = j::Value(status);
    row["vram_mb"] = j::Value(0.0);
    row["error"] = j::Value("load_failed");
    probe({j::Value(std::move(row))}, true);
  }
  for (const auto& [key, unused] : canonical) {
    auto row = canonical;
    row.erase(key);
    probe({j::Value(std::move(row))}, key == "completedAt");
  }
  for (const auto* key : {"p50_ms", "p95_ms", "p99_ms", "load_ms", "rss_mb", "vram_mb"}) {
    auto row = canonical;
    row[key] = j::Value(-1.0);
    probe({j::Value(std::move(row))}, false);
  }
  for (const auto* timestamp : {"2026-02-30T10:00:00Z", "2026-10-10T24:00:00Z",
                                "2026-10-10T10:00:00+00:00", "2026-10-10T10:00:00.001Z"}) {
    auto row = canonical;
    row["completedAt"] = j::Value(timestamp);
    probe({j::Value(std::move(row))}, false);
  }
  for (const auto* model_path : {"", "model.gguf", "C:model.gguf"}) {
    auto row = canonical;
    row["path"] = j::Value(model_path);
    probe({j::Value(std::move(row))}, false);
  }
  for (const double iterations : {-1.0, 4294967296.0}) {
    auto row = canonical;
    row["iterations_completed"] = j::Value(iterations);
    probe({j::Value(std::move(row))}, false);
  }
  for (const auto& [key, invalid] : j::Object{{"backend", j::Value("auto")},
                                              {"status", j::Value("unknown")},
                                              {"iterations_completed", j::Value(0.5)},
                                              {"error", j::Value(true)},
                                              {"vram_mb", j::Value("unknown")},
                                              {"extra", j::Value(true)}}) {
    auto row = canonical;
    row[key] = invalid;
    probe({j::Value(std::move(row))}, false);
  }
  std::filesystem::remove_all(directory);
}

TEST(DiagnosticsTest, EmbeddedSettingsSchemaRejectsUnknownAndOutOfRangeInferenceSettings) {
  const auto path = TempPath("azookey_diagnostics_inference_settings.json");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << R"({"unknownInferenceSetting":1})";
  }
  EXPECT_FALSE(diag::ProbeSettingsFile(path));

  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << R"({"inferenceThreads":9,"maxCandidates":33,"maxContextLength":31})";
  }
  EXPECT_FALSE(diag::ProbeSettingsFile(path));

  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << R"({"inferenceThreads":8,"maxCandidates":32,"maxContextLength":30})";
  }
  EXPECT_TRUE(diag::ProbeSettingsFile(path));
  std::filesystem::remove(path);
}

diag::Status SettingsCheckStatus(bool settings_valid, bool settings_missing,
                                 bool settings_migration_available = false) {
  diag::Snapshot snapshot;
  snapshot.settings_valid = settings_valid;
  snapshot.settings_missing = settings_missing;
  snapshot.settings_migration_available = settings_migration_available;
  const auto report = diag::EvaluateSnapshot(snapshot, 1);
  const auto check = std::find_if(report.checks.begin(), report.checks.end(),
                                  [](const auto& item) { return item.id == "D-012"; });
  if (check == report.checks.end()) throw std::runtime_error("D-012 check is missing");
  return check->status;
}

TEST(DiagnosticsTest, RegisteredLegacyBenchmarkHistoryWarnsWithoutHidingOtherInvalidSettings) {
  const auto directory = UniqueTempDirectory("azookey-diag-legacy-benchmark-history-");
  const auto path = directory / "settings.json";
  const auto probe = [&](const std::string& text, bool expected_valid, bool expected_migration) {
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << text;
    }
    bool migration = true;
    const bool valid = diag::ProbeSettingsFile(path, &migration);
    EXPECT_EQ(valid, expected_valid) << text;
    EXPECT_EQ(migration, expected_migration) << text;
    EXPECT_EQ(SettingsCheckStatus(valid, false, migration),
              !expected_valid ? diag::Status::Error
                              : (expected_migration ? diag::Status::Warning : diag::Status::Ok));
    MockDpapiCrypto crypto;
    EXPECT_EQ(diag::ProbeDpapiSettingsJson(text, crypto),
              expected_valid ? diag::DpapiState::NotRequired : diag::DpapiState::Unavailable);
    std::ifstream in(path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), text);
  };
  for (const auto* identifier : {"model", "model_path", "path"}) {
    probe(std::string(R"({"model":{"benchmarkHistory":[{")") + identifier +
              R"(":"legacy.gguf","backend":"cpu","p50_ms":1}]}})",
          true, true);
  }
  const j::Object legacy{{"path", j::Value("legacy.gguf")},
                         {"backend", j::Value("cpu")},
                         {"status", j::Value("error")},
                         {"p50_ms", j::Value{}},
                         {"p95_ms", j::Value(1.0)},
                         {"p99_ms", j::Value(2.0)},
                         {"load_ms", j::Value(3.0)},
                         {"rss_mb", j::Value(4.0)},
                         {"vram_mb", j::Value{}},
                         {"iterations_completed", j::Value(0.0)},
                         {"error", j::Value{}}};
  const auto document = [](j::Array rows) {
    return j::Stringify(j::Value(j::Object{
        {"model", j::Value(j::Object{{"benchmarkHistory", j::Value(std::move(rows))}})}}));
  };
  probe(document({j::Value(legacy)}), true, true);
  for (const auto* key :
       {"p50_ms", "p95_ms", "p99_ms", "load_ms", "rss_mb", "vram_mb", "iterations_completed"}) {
    auto row = legacy;
    row[key] = j::Value{};
    probe(document({j::Value(std::move(row))}), true, true);
  }
  for (const auto& [key, invalid] : j::Object{{"load_ms", j::Value("invalid")},
                                              {"rss_mb", j::Value(true)},
                                              {"error", j::Value(1.0)},
                                              {"iterations_completed", j::Value(0.5)},
                                              {"vram_mb", j::Value(false)}}) {
    auto row = legacy;
    row[key] = invalid;
    probe(document({j::Value(std::move(row))}), false, false);
  }
  for (const double iterations : {-1.0, 4294967296.0}) {
    auto row = legacy;
    row["iterations_completed"] = j::Value(iterations);
    probe(document({j::Value(std::move(row))}), false, false);
  }
  auto canonical = legacy;
  canonical["path"] = j::Value("C:/models/new.gguf");
  canonical["completedAt"] = j::Value("2026-10-10T10:00:00Z");
  canonical["p50_ms"] = j::Value(0.0);
  probe(document({j::Value(legacy), j::Value(canonical)}), true, true);
  for (const auto& timestamp :
       {j::Value("2026-10-10T10:00:00Z"), j::Value("invalid"), j::Value{}, j::Value(1.0)}) {
    auto row = canonical;
    row["completedAt"] = timestamp;
    row.erase("load_ms");
    probe(document({j::Value(legacy), j::Value(std::move(row))}), false, false);
  }
  auto effective_openai = j::Parse(document({j::Value(legacy)}));
  ASSERT_TRUE(effective_openai);
  auto openai_document = effective_openai->AsObject();
  openai_document["aiBackend"] = j::Value("openai");
  openai_document["openAiApiKey"] = j::Value("legacy-key");
  MockDpapiCrypto crypto;
  EXPECT_EQ(
      diag::ProbeDpapiSettingsJson(j::Stringify(j::Value(std::move(openai_document))), crypto),
      diag::DpapiState::Plaintext);
  probe(R"({"model":{"benchmarkHistory":[]}})", true, false);
  probe(R"({"maxCandidates":99,"model":{"benchmarkHistory":[{"model":"legacy"}]}})", false, false);
  probe(R"({"unknownSetting":true,"model":{"benchmarkHistory":[{"model":"legacy"}]}})", false,
        false);
  probe(R"({"model":{"selectedPath":42,"benchmarkHistory":[{"model":"legacy"}]}})", false, false);
  probe(R"({"model":{"benchmarkHistory":[{"model":"legacy","extra":true}]}})", false, false);
  probe(R"({"model":{"benchmarkHistory":[{"path":"legacy","completedAt":"invalid"}]}})", false,
        false);
  probe(R"({"model":{"benchmarkHistory":[{"model":"legacy"},{"path":"new","rss_mb":"invalid"}]}})",
        false, false);
  std::filesystem::remove_all(directory);
}

TEST(DiagnosticsTest, BackwardCompatibleSettingsAreOkAndProbeLeavesFileUnchanged) {
  const auto directory = UniqueTempDirectory("azookey-diag-compatible-settings-");
  const auto path = directory / "settings.json";
  const std::string text = R"({"backendPreference":"directml","epPreference":"npu",)"
                           R"("promptPrefixByApp":{"Code.exe":"code"},"aiBackend":"openai",)"
                           R"("openAiApiKey":"plaintext-key"})";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
  }
  const bool valid = diag::ProbeSettingsFile(path);
  EXPECT_TRUE(valid);
  EXPECT_EQ(SettingsCheckStatus(valid, false), diag::Status::Ok);
  std::ifstream in(path, std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), text);
  in.close();
  EXPECT_EQ(SettingsCheckStatus(true, true), diag::Status::Ok);
  std::filesystem::remove_all(directory);
}

TEST(DiagnosticsTest, UnregisteredLegacyLookingSettingsAreErrorNotWarning) {
  const auto directory = UniqueTempDirectory("azookey-diag-unregistered-settings-");
  const auto path = directory / "settings.json";
  for (const char* text :
       {R"({"schemaVersion":1,"backendPreference":"auto"})",
        R"({"settings":{"backendPreference":"auto"}})", R"({"backendPreference":"directml")"}) {
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << text;
    }
    const bool valid = diag::ProbeSettingsFile(path);
    EXPECT_FALSE(valid) << text;
    EXPECT_EQ(SettingsCheckStatus(valid, false), diag::Status::Error) << text;
  }
  std::filesystem::remove_all(directory);
}

}  // namespace
