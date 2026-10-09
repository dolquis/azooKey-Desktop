#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "ProofreadModel.h"
#include "SettingsIpcClient.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"

namespace {

azookey::ipc::Envelope ResponseFor(const azookey::ipc::Envelope& request, std::string payload) {
  azookey::ipc::Envelope response;
  response.request_id = request.request_id;
  response.trace_id = request.trace_id;
  response.type = request.type;
  response.payload_json = std::move(payload);
  return response;
}

}  // namespace

TEST(SettingsIpcClientTest, HandshakesThenSendsEmptyUpdateConfigPayload) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::string pipe_name = "\\\\.\\pipe\\azookey-settings-test-" +
                                std::to_string(GetCurrentProcessId()) + "-" + std::to_string(stamp);
  std::atomic<bool> saw_empty_update{false};
  std::atomic<bool> authenticated{false};
  std::atomic<bool> shared_trace{false};
  std::string handshake_trace;

  azookey::ipc::NamedPipeServer server;
  ASSERT_TRUE(server.Start(pipe_name, [&] {
    return [&](const azookey::ipc::Envelope& request) -> std::optional<azookey::ipc::Envelope> {
      if (request.type == azookey::ipc::MessageType::Handshake) {
        handshake_trace = request.trace_id;
        const auto parsed = azookey::ipc::ParseHandshakeRequest(request.payload_json);
        authenticated = parsed && parsed->tip_version == "settings-app" &&
                        parsed->protocol_version == azookey::ipc::kHandshakeProtocolVersion &&
                        parsed->capabilities == std::vector<std::string>{"settings"};
        azookey::ipc::HandshakeResponse response;
        response.host_version = "test";
        response.accepted = authenticated;
        return ResponseFor(request, azookey::ipc::BuildHandshakeResponse(response));
      }
      if (request.type == azookey::ipc::MessageType::UpdateConfig && authenticated) {
        shared_trace = request.trace_id == handshake_trace && request.trace_id.size() == 36 &&
                       request.trace_id[14] == '7';
        saw_empty_update = request.payload_json == "{}";
        azookey::ipc::UpdateConfigResponse response;
        response.ok = true;
        return ResponseFor(request, azookey::ipc::BuildUpdateConfigResponse(response));
      }
      return std::nullopt;
    };
  }));

  azookey::settings::SettingsIpcOptions options;
  options.pipe_name = pipe_name;
  options.handshake_token = "settings-test-token";
  options.connect_timeout_ms = 1000;
  options.response_timeout_ms = 1000;
  const auto result = azookey::settings::NotifyHostOfSettingsChange(options);
  server.Stop();

  EXPECT_TRUE(result.ok) << result.error.value_or("");
  EXPECT_TRUE(authenticated);
  EXPECT_TRUE(saw_empty_update);
  EXPECT_TRUE(shared_trace);
}

namespace {

// A Host that accepts the Handshake, advertises `capabilities`, and answers each other request
// with whatever `reply` returns for its type and payload.
class FakeHost {
 public:
  using Reply =
      std::function<std::optional<std::string>(azookey::ipc::MessageType, const std::string&)>;

  FakeHost(std::vector<std::string> capabilities, Reply reply)
      : capabilities_(std::move(capabilities)), reply_(std::move(reply)) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    pipe_name_ = "\\\\.\\pipe\\azookey-settings-call-test-" +
                 std::to_string(GetCurrentProcessId()) + "-" + std::to_string(stamp);
    started_ = server_.Start(pipe_name_, [this] {
      return
          [this](const azookey::ipc::Envelope& request) -> std::optional<azookey::ipc::Envelope> {
            if (request.type == azookey::ipc::MessageType::Handshake) {
              azookey::ipc::HandshakeResponse response;
              response.host_version = "test";
              response.accepted = true;
              response.capabilities = capabilities_;
              return ResponseFor(request, azookey::ipc::BuildHandshakeResponse(response));
            }
            ++requests_;
            const auto payload = reply_(request.type, request.payload_json);
            if (!payload) return std::nullopt;
            return ResponseFor(request, *payload);
          };
    });
  }

  ~FakeHost() { server_.Stop(); }

  bool started() const { return started_; }
  int requests() const { return requests_; }

  azookey::settings::SettingsIpcOptions Options() const {
    azookey::settings::SettingsIpcOptions options;
    options.pipe_name = pipe_name_;
    options.handshake_token = "settings-test-token";
    options.connect_timeout_ms = 1000;
    options.response_timeout_ms = 1000;
    options.long_response_timeout_ms = 1000;
    return options;
  }

 private:
  std::vector<std::string> capabilities_;
  Reply reply_;
  std::string pipe_name_;
  azookey::ipc::NamedPipeServer server_;
  bool started_{false};
  std::atomic<int> requests_{0};
};

using azookey::settings::HostCallStatus;

}  // namespace

TEST(SettingsIpcClientTest, ListModelsSendsTheRequestAndParsesTheModels) {
  std::string seen_request;
  FakeHost host({"list_models"}, [&](azookey::ipc::MessageType type, const std::string& payload) {
    EXPECT_EQ(type, azookey::ipc::MessageType::ListModels);
    seen_request = payload;
    azookey::ipc::ListModelsResponse response;
    azookey::ipc::ListedModel model;
    model.path = "C:\\models\\zenz.gguf";
    model.file_name = "zenz.gguf";
    model.format = "gguf";
    model.size_bytes = 1234;
    model.valid = true;
    model.last_load_status = "success";
    response.models.push_back(model);
    return std::optional<std::string>(azookey::ipc::BuildListModelsResponse(response));
  });
  ASSERT_TRUE(host.started());

  azookey::ipc::ListModelsRequest request;
  request.compute_sha256 = true;
  const auto result = azookey::settings::RequestListModels(host.Options(), request);

  ASSERT_EQ(result.status, HostCallStatus::Ok);
  ASSERT_TRUE(result.response);
  ASSERT_EQ(result.response->models.size(), 1U);
  EXPECT_EQ(result.response->models[0].file_name, "zenz.gguf");
  EXPECT_TRUE(result.response->models[0].valid);
  const auto parsed = azookey::ipc::ParseListModelsRequest(seen_request);
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->compute_sha256);
}

TEST(SettingsIpcClientTest, BenchmarkModelCarriesTheRequestAndTheMeasurements) {
  std::optional<azookey::ipc::BenchmarkModelRequest> seen;
  FakeHost host({"benchmark_model"}, [&](azookey::ipc::MessageType, const std::string& payload) {
    seen = azookey::ipc::ParseBenchmarkModelRequest(payload);
    azookey::ipc::BenchmarkModelResponse response;
    response.backend = "cpu";
    response.p50_ms = 18.5;
    response.p95_ms = 41.0;
    response.status = "success";
    response.iterations_completed = 7;
    return std::optional<std::string>(azookey::ipc::BuildBenchmarkModelResponse(response));
  });
  ASSERT_TRUE(host.started());

  azookey::ipc::BenchmarkModelRequest request;
  request.path = "C:\\models\\zenz.gguf";
  request.backend = "cpu";
  request.iterations = 7;
  const auto result = azookey::settings::RequestBenchmarkModel(host.Options(), request);

  ASSERT_EQ(result.status, HostCallStatus::Ok);
  ASSERT_TRUE(result.response);
  EXPECT_EQ(result.response->status, "success");
  EXPECT_EQ(result.response->iterations_completed, 7U);
  EXPECT_DOUBLE_EQ(result.response->p50_ms, 18.5);
  ASSERT_TRUE(seen);
  EXPECT_EQ(seen->path, "C:\\models\\zenz.gguf");
  EXPECT_EQ(seen->iterations, 7U);
}

TEST(SettingsIpcClientTest, HostErrorsInThePayloadAreReturnedNotHidden) {
  FakeHost host({"list_models"}, [](azookey::ipc::MessageType, const std::string&) {
    azookey::ipc::ListModelsResponse response;
    response.ok = false;
    response.error = "models_dir_unavailable";
    return std::optional<std::string>(azookey::ipc::BuildListModelsResponse(response));
  });
  ASSERT_TRUE(host.started());

  const auto result = azookey::settings::RequestListModels(host.Options(), {});

  ASSERT_EQ(result.status, HostCallStatus::Ok);
  ASSERT_TRUE(result.response);
  EXPECT_FALSE(result.response->ok);
  EXPECT_EQ(result.response->error, "models_dir_unavailable");
}

TEST(SettingsIpcClientTest, AHostWithoutTheCapabilityIsNeverSentTheRequest) {
  FakeHost host({"settings"}, [](azookey::ipc::MessageType, const std::string&) {
    return std::optional<std::string>("{}");
  });
  ASSERT_TRUE(host.started());

  EXPECT_EQ(azookey::settings::RequestListModels(host.Options(), {}).status,
            HostCallStatus::Unsupported);
  EXPECT_EQ(azookey::settings::RequestBenchmarkModel(host.Options(), {}).status,
            HostCallStatus::Unsupported);
  EXPECT_EQ(azookey::settings::RequestListLearningEntries(host.Options(), {}).status,
            HostCallStatus::Unsupported);
  EXPECT_EQ(host.requests(), 0);
}

TEST(SettingsIpcClientTest, LearningDataMessagesRoundTrip) {
  std::vector<azookey::ipc::MessageType> seen;
  FakeHost host({"learning_data_management"}, [&](azookey::ipc::MessageType type,
                                                  const std::string&) {
    seen.push_back(type);
    switch (type) {
      case azookey::ipc::MessageType::ListLearningEntries: {
        azookey::ipc::ListLearningEntriesResponse response;
        response.total = 1;
        azookey::ipc::LearningEntryField entry;
        entry.id = "0123456789abcdef";
        entry.reading = "reading";
        entry.surface = "surface";
        entry.weight = 4.2;
        response.entries.push_back(entry);
        return std::optional<std::string>(azookey::ipc::BuildListLearningEntriesResponse(response));
      }
      case azookey::ipc::MessageType::ForgetLearningEntry: {
        azookey::ipc::ForgetLearningEntryResponse response;
        response.removed = true;
        return std::optional<std::string>(azookey::ipc::BuildForgetLearningEntryResponse(response));
      }
      case azookey::ipc::MessageType::ExportLearningData: {
        azookey::ipc::ExportLearningDataResponse response;
        response.status = "success";
        response.file_size_bytes = 99;
        return std::optional<std::string>(azookey::ipc::BuildExportLearningDataResponse(response));
      }
      default: {
        azookey::ipc::ImportLearningDataResponse response;
        response.status = "success";
        response.imported_counts["learning"] = 3;
        return std::optional<std::string>(azookey::ipc::BuildImportLearningDataResponse(response));
      }
    }
  });
  ASSERT_TRUE(host.started());
  const auto options = host.Options();

  azookey::ipc::ListLearningEntriesRequest list;
  list.store = "learning";
  const auto listed = azookey::settings::RequestListLearningEntries(options, list);
  ASSERT_EQ(listed.status, HostCallStatus::Ok);
  ASSERT_EQ(listed.response->entries.size(), 1U);
  EXPECT_EQ(listed.response->entries[0].surface, "surface");

  azookey::ipc::ForgetLearningEntryRequest forget;
  forget.store = "learning";
  forget.id = "0123456789abcdef";
  const auto forgotten = azookey::settings::RequestForgetLearningEntry(options, forget);
  ASSERT_EQ(forgotten.status, HostCallStatus::Ok);
  EXPECT_TRUE(forgotten.response->removed);

  azookey::ipc::ExportLearningDataRequest export_request;
  export_request.stores = {"learning"};
  export_request.destination_path = "C:\\backup.zip";
  const auto exported = azookey::settings::RequestExportLearningData(options, export_request);
  ASSERT_EQ(exported.status, HostCallStatus::Ok);
  EXPECT_EQ(exported.response->status, "success");

  azookey::ipc::ImportLearningDataRequest import_request;
  import_request.source_path = "C:\\backup.zip";
  const auto imported = azookey::settings::RequestImportLearningData(options, import_request);
  ASSERT_EQ(imported.status, HostCallStatus::Ok);
  EXPECT_EQ(imported.response->imported_counts.at("learning"), 3U);

  EXPECT_EQ(seen.size(), 4U);
}

TEST(SettingsIpcClientTest, ResetLearningStoreSendsTheStoreAndNeedsItsCapability) {
  std::optional<azookey::ipc::ResetLearningStoreRequest> seen;
  FakeHost host(
      {"learning_reset"}, [&](azookey::ipc::MessageType type, const std::string& payload) {
        EXPECT_EQ(type, azookey::ipc::MessageType::ResetLearningStore);
        seen = azookey::ipc::ParseResetLearningStoreRequest(payload);
        azookey::ipc::ResetLearningStoreResponse response;
        response.ok = false;
        response.error = "save_failed";
        return std::optional<std::string>(azookey::ipc::BuildResetLearningStoreResponse(response));
      });
  ASSERT_TRUE(host.started());
  azookey::ipc::ResetLearningStoreRequest request;
  request.store = "auto_word";

  const auto result = azookey::settings::RequestResetLearningStore(host.Options(), request);

  ASSERT_EQ(result.status, HostCallStatus::Ok);
  EXPECT_FALSE(result.response->ok);
  EXPECT_EQ(result.response->error, "save_failed");
  ASSERT_TRUE(seen);
  EXPECT_EQ(seen->store, "auto_word");

  FakeHost older({"learning_data_management"}, [](azookey::ipc::MessageType, const std::string&) {
    return std::optional<std::string>("{}");
  });
  ASSERT_TRUE(older.started());
  EXPECT_EQ(azookey::settings::RequestResetLearningStore(older.Options(), request).status,
            HostCallStatus::Unsupported);
  EXPECT_EQ(older.requests(), 0);
}

TEST(SettingsIpcClientTest, ProbingACapabilitySendsNoRequest) {
  FakeHost host({"learning_reset"}, [](azookey::ipc::MessageType, const std::string&) {
    return std::optional<std::string>("{}");
  });
  ASSERT_TRUE(host.started());

  EXPECT_EQ(azookey::settings::ProbeHostCapability(host.Options(), "learning_reset"),
            HostCallStatus::Ok);
  EXPECT_EQ(azookey::settings::ProbeHostCapability(host.Options(), "persona"),
            HostCallStatus::Unsupported);
  EXPECT_EQ(host.requests(), 0);

  azookey::settings::SettingsIpcOptions absent;
  absent.pipe_name = "\\\\.\\pipe\\azookey-settings-probe-absent";
  absent.handshake_token = "settings-test-token";
  absent.connect_timeout_ms = 100;
  EXPECT_EQ(azookey::settings::ProbeHostCapability(absent, "learning_reset"),
            HostCallStatus::HostNotRunning);
}

TEST(SettingsIpcClientTest, QueryPersonaSendsAnEmptyObjectAndNeedsItsCapability) {
  std::string seen_payload;
  FakeHost host({"persona"}, [&](azookey::ipc::MessageType type, const std::string& payload) {
    EXPECT_EQ(type, azookey::ipc::MessageType::QueryPersona);
    seen_payload = payload;
    azookey::ipc::QueryPersonaResponse response;
    response.polite_ratio = 0.5;
    response.sample_count = 12;
    response.computed_at_epoch_sec = 1780000000;
    return std::optional<std::string>(azookey::ipc::BuildQueryPersonaResponse(response));
  });
  ASSERT_TRUE(host.started());

  const auto result = azookey::settings::RequestQueryPersona(host.Options());

  ASSERT_EQ(result.status, HostCallStatus::Ok);
  EXPECT_EQ(seen_payload, "{}");
  EXPECT_DOUBLE_EQ(result.response->polite_ratio, 0.5);
  EXPECT_EQ(result.response->sample_count, 12u);

  FakeHost older({"settings"}, [](azookey::ipc::MessageType, const std::string&) {
    return std::optional<std::string>("{}");
  });
  ASSERT_TRUE(older.started());
  EXPECT_EQ(azookey::settings::RequestQueryPersona(older.Options()).status,
            HostCallStatus::Unsupported);
}

TEST(SettingsIpcClientTest, QueryDiagnosticsReadsTheNeologdLayerWhenTheHostSendsIt) {
  FakeHost host({"settings"}, [](azookey::ipc::MessageType type, const std::string&) {
    EXPECT_EQ(type, azookey::ipc::MessageType::QueryDiagnostics);
    azookey::ipc::QueryDiagnosticsPayload diagnostics;
    azookey::ipc::NeologdLayerStatus layer;
    layer.state = "error";
    layer.reason = "verify_failed";
    diagnostics.neologd_layer = layer;
    return std::optional<std::string>(azookey::ipc::BuildQueryDiagnostics(diagnostics));
  });
  ASSERT_TRUE(host.started());

  const auto result = azookey::settings::RequestQueryDiagnostics(host.Options());

  ASSERT_EQ(result.status, HostCallStatus::Ok);
  ASSERT_TRUE(result.response->neologd_layer);
  EXPECT_EQ(result.response->neologd_layer->state, "error");
  EXPECT_EQ(result.response->neologd_layer->reason, "verify_failed");
}

TEST(SettingsIpcClientTest, QueryDiagnosticsFromAHostWithoutTheFieldHasNoLayer) {
  FakeHost host({"settings"}, [](azookey::ipc::MessageType, const std::string&) {
    return std::optional<std::string>(
        azookey::ipc::BuildQueryDiagnostics(azookey::ipc::QueryDiagnosticsPayload{}));
  });
  ASSERT_TRUE(host.started());

  const auto result = azookey::settings::RequestQueryDiagnostics(host.Options());

  ASSERT_EQ(result.status, HostCallStatus::Ok);
  EXPECT_FALSE(result.response->neologd_layer);
}

TEST(SettingsIpcClientTest, DetectAnomaliesCarriesTheRequestAndTheFindings) {
  std::optional<azookey::ipc::DetectAnomaliesRequest> seen;
  FakeHost host(
      {"detect_anomalies"}, [&](azookey::ipc::MessageType type, const std::string& payload) {
        EXPECT_EQ(type, azookey::ipc::MessageType::DetectAnomalies);
        seen = azookey::ipc::ParseDetectAnomaliesRequest(payload);
        azookey::ipc::DetectAnomaliesResponse response;
        azookey::ipc::AnomalyFindingField finding;
        finding.start = 2;
        finding.length = 3;
        finding.reason = "reason <b>text</b>";
        finding.suggestions = {"one", "two"};
        finding.confidence = 0.75;
        response.findings.push_back(finding);
        return std::optional<std::string>(azookey::ipc::BuildDetectAnomaliesResponse(response));
      });
  ASSERT_TRUE(host.started());

  const auto result = azookey::settings::RequestDetectAnomalies(
      host.Options(), azookey::settings::MakeProofreadRequest("pasted text"));

  ASSERT_EQ(result.status, HostCallStatus::Ok);
  ASSERT_TRUE(seen);
  EXPECT_EQ(seen->text, "pasted text");
  EXPECT_FALSE(seen->secure);
  EXPECT_TRUE(seen->learning_allowed);
  ASSERT_EQ(result.response->findings.size(), 1u);
  const auto& finding = result.response->findings[0];
  EXPECT_EQ(finding.start, 2u);
  EXPECT_EQ(finding.length, 3u);
  // The AI's words arrive as they are; the pane shows them as plain text.
  EXPECT_EQ(finding.reason, "reason <b>text</b>");
  EXPECT_EQ(finding.suggestions.size(), 2u);
}

TEST(SettingsIpcClientTest, DetectAnomaliesReturnsTheHostsRefusalAndNeedsItsCapability) {
  FakeHost host({"detect_anomalies"}, [](azookey::ipc::MessageType, const std::string&) {
    azookey::ipc::DetectAnomaliesResponse response;
    response.ok = false;
    response.error = "unsupported";
    return std::optional<std::string>(azookey::ipc::BuildDetectAnomaliesResponse(response));
  });
  ASSERT_TRUE(host.started());
  const auto refused = azookey::settings::RequestDetectAnomalies(
      host.Options(), azookey::settings::MakeProofreadRequest("text"));
  ASSERT_EQ(refused.status, HostCallStatus::Ok);
  EXPECT_FALSE(refused.response->ok);
  EXPECT_EQ(refused.response->error, "unsupported");

  FakeHost older({"persona"}, [](azookey::ipc::MessageType, const std::string&) {
    return std::optional<std::string>("{}");
  });
  ASSERT_TRUE(older.started());
  EXPECT_EQ(azookey::settings::RequestDetectAnomalies(
                older.Options(), azookey::settings::MakeProofreadRequest("text"))
                .status,
            HostCallStatus::Unsupported);
  EXPECT_EQ(older.requests(), 0);
}

TEST(SettingsIpcClientTest, AnUnparsableResponseIsReportedAsInvalid) {
  FakeHost host({"list_models"}, [](azookey::ipc::MessageType, const std::string&) {
    return std::optional<std::string>("[]");
  });
  ASSERT_TRUE(host.started());

  EXPECT_EQ(azookey::settings::RequestListModels(host.Options(), {}).status,
            HostCallStatus::InvalidResponse);
}

TEST(SettingsIpcClientTest, ASilentHostTimesOut) {
  FakeHost host({"list_models"}, [](azookey::ipc::MessageType, const std::string&) {
    return std::optional<std::string>();
  });
  ASSERT_TRUE(host.started());
  auto options = host.Options();
  options.response_timeout_ms = 200;

  EXPECT_EQ(azookey::settings::RequestListModels(options, {}).status, HostCallStatus::Timeout);
}

TEST(SettingsIpcClientTest, ANotRunningHostIsReportedAsSuch) {
  azookey::settings::SettingsIpcOptions options;
  options.pipe_name =
      "\\\\.\\pipe\\azookey-settings-absent-" + std::to_string(GetCurrentProcessId());
  options.handshake_token = "settings-test-token";
  options.connect_timeout_ms = 100;

  EXPECT_EQ(azookey::settings::RequestListModels(options, {}).status,
            HostCallStatus::HostNotRunning);
}
