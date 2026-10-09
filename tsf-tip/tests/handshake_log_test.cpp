#include <Windows.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/tsf/TextService.h"
#include "azookey/tsf/TipRuntimeLog.h"

// The TIP's handshake records must not carry the Handshake token, whatever the
// outcome (DEV-1171 acceptance 3, DEV-1507). The TIP logger is routed to a
// temporary file for the test; nothing is written under LocalAppData.

namespace {

using namespace azookey::ipc;

constexpr char kHostToken[] = "tip-log-host-token-7d41c0e9b2a8";
constexpr char kWrongToken[] = "tip-log-wrong-token-3a96f5d27c1e";

class ScopedHandshakeToken {
 public:
  explicit ScopedHandshakeToken(const char* value) {
    char* prior = nullptr;
    size_t length = 0;
    if (_dupenv_s(&prior, &length, "AZOOKEY_IPC_HANDSHAKE_TOKEN") == 0 && prior) original_ = prior;
    std::free(prior);
    EXPECT_EQ(_putenv_s("AZOOKEY_IPC_HANDSHAKE_TOKEN", value), 0);
  }
  ~ScopedHandshakeToken() { _putenv_s("AZOOKEY_IPC_HANDSHAKE_TOKEN", original_.c_str()); }

 private:
  std::string original_;
};

// Owns the temporary log and the logger override. Declared before the
// TextService so the service, which keeps a pointer to the logger, goes first.
class ScopedTipLog {
 public:
  ScopedTipLog() {
    static std::atomic<unsigned> sequence{0};
    root_ = std::filesystem::temp_directory_path() /
            (L"azookey-tip-handshake-log-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++));
    std::filesystem::create_directories(root_);
    azookey::logging::RuntimeLoggerOptions options;
    options.enabled = true;
    // Even the body opt-in must not let the token through.
    options.body_opt_in = true;
    options.component = "tip";
    options.logs_directory = root_;
    options.output_path = root_ / L"tip.jsonl";
    logger_ = std::make_unique<azookey::logging::RuntimeLogger>(options);
    azookey::tsf::SetTipRuntimeLoggerForTest(logger_.get());
  }
  ~ScopedTipLog() {
    azookey::tsf::SetTipRuntimeLoggerForTest(nullptr);
    logger_.reset();
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  std::string Read() const {
    std::ifstream in(root_ / L"tip.jsonl", std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  // Waits for a record containing every fragment, returning the whole log.
  std::string WaitFor(std::initializer_list<std::string> fragments) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::string log;
    while (std::chrono::steady_clock::now() < deadline) {
      log = Read();
      size_t start = 0;
      while (start < log.size()) {
        size_t end = log.find('\n', start);
        if (end == std::string::npos) end = log.size();
        const std::string line = log.substr(start, end - start);
        bool all = true;
        for (const auto& fragment : fragments)
          all = all && line.find(fragment) != std::string::npos;
        if (all) return log;
        start = end + 1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ADD_FAILURE() << "log record not found; log:\n" << log;
    return log;
  }

 private:
  std::filesystem::path root_;
  std::unique_ptr<azookey::logging::RuntimeLogger> logger_;
};

std::string UniquePipeName(const char* label) {
  static std::atomic<unsigned> sequence{0};
  return std::string("\\\\.\\pipe\\azookey-tip-handshake-log-") + label + "-" +
         std::to_string(GetCurrentProcessId()) + "-" + std::to_string(sequence++);
}

// A Host that accepts only kHostToken. `batch_after_first` makes every
// handshake after the first report batchRomajiConversion, as a settings save
// read by a refresh handshake would.
bool StartHost(NamedPipeServer& server, const std::string& pipe_name, std::atomic<int>& handshakes,
               bool batch_after_first = false) {
  return server.Start(
      pipe_name,
      [&handshakes, batch_after_first](const Envelope& request) -> std::optional<Envelope> {
        if (request.type != MessageType::Handshake) return std::nullopt;
        const auto parsed = ParseHandshakeRequest(request.payload_json);
        const int count = ++handshakes;
        HandshakeResponse payload;
        payload.host_version = "test-host";
        payload.host_generation_id = "test-generation";
        payload.accepted = parsed && parsed->handshake_token == kHostToken;
        payload.batch_romaji_conversion = batch_after_first && count > 1;
        Envelope response = request;
        response.payload_json = BuildHandshakeResponse(payload);
        return response;
      });
}

void ExpectNoToken(const std::string& log) {
  EXPECT_EQ(log.find(kHostToken), std::string::npos) << log;
  EXPECT_EQ(log.find(kWrongToken), std::string::npos) << log;
}

TEST(TsfTipHandshakeLogTest, AcceptedHandshakeLogsWithoutToken) {
  ScopedHandshakeToken token(kHostToken);
  ScopedTipLog log;
  const auto pipe_name = UniquePipeName("accepted");
  std::atomic<int> handshakes{0};
  NamedPipeServer server;
  ASSERT_TRUE(StartHost(server, pipe_name, handshakes));
  {
    azookey::tsf::TextService service;
    service.set_ipc_pipe_name_for_test(pipe_name);
    service.start_ipc_worker_for_test();
    const auto text = log.WaitFor({"\"event\":\"ipc_connected\""});
    service.stop_ipc_worker_for_test();
    ExpectNoToken(text);
  }
  ExpectNoToken(log.Read());
}

TEST(TsfTipHandshakeLogTest, RejectedTokenLogsWithoutEitherToken) {
  ScopedHandshakeToken token(kWrongToken);
  ScopedTipLog log;
  const auto pipe_name = UniquePipeName("rejected");
  std::atomic<int> handshakes{0};
  NamedPipeServer server;
  ASSERT_TRUE(StartHost(server, pipe_name, handshakes));
  {
    azookey::tsf::TextService service;
    service.set_ipc_pipe_name_for_test(pipe_name);
    service.start_ipc_worker_for_test();
    const auto text = log.WaitFor({"\"event\":\"ipc_handshake_rejected\""});
    service.stop_ipc_worker_for_test();
    EXPECT_EQ(text.find("\"event\":\"ipc_connected\""), std::string::npos) << text;
    ExpectNoToken(text);
  }
  ExpectNoToken(log.Read());
}

TEST(TsfTipHandshakeLogTest, MissingTokenLogsUnavailableWithoutToken) {
  // The environment still holds a token, so a leak through the wrong path
  // would show up in the log.
  ScopedHandshakeToken token(kHostToken);
  ScopedTipLog log;
  const auto pipe_name = UniquePipeName("missing");
  std::atomic<int> handshakes{0};
  NamedPipeServer server;
  ASSERT_TRUE(StartHost(server, pipe_name, handshakes));
  {
    azookey::tsf::TextService service;
    service.set_handshake_token_unavailable_for_test(true);
    service.set_ipc_pipe_name_for_test(pipe_name);
    service.start_ipc_worker_for_test();
    const auto text = log.WaitFor({"\"event\":\"ipc_handshake_token_unavailable\""});
    service.stop_ipc_worker_for_test();
    EXPECT_EQ(handshakes.load(), 0);
    ExpectNoToken(text);
  }
  ExpectNoToken(log.Read());
}

// DEV-1493: the log tells which batch mode a TIP serves keys with, and whether
// a settings refresh replaced the one from activation.
TEST(TsfTipHandshakeLogTest, RefreshLogsTheBatchModeItApplied) {
  ScopedHandshakeToken token(kHostToken);
  ScopedTipLog log;
  const auto pipe_name = UniquePipeName("refresh");
  std::atomic<int> handshakes{0};
  NamedPipeServer server;
  ASSERT_TRUE(StartHost(server, pipe_name, handshakes, /*batch_after_first=*/true));
  {
    azookey::tsf::TextService service;
    service.set_ipc_pipe_name_for_test(pipe_name);
    service.start_ipc_worker_for_test();
    log.WaitFor({"\"event\":\"ipc_connected\"", "\"batch_romaji_conversion\":false"});
    EXPECT_FALSE(service.batch_romaji_conversion_for_test());
    service.request_host_option_refresh_for_test();
    log.WaitFor({"\"event\":\"ipc_host_options_refreshed\"", "\"batch_romaji_conversion\":true"});
    const auto text =
        log.WaitFor({"\"event\":\"ipc_connected\"", "\"batch_romaji_conversion\":true"});
    service.stop_ipc_worker_for_test();
    EXPECT_TRUE(service.batch_romaji_conversion_for_test());
    EXPECT_NE(text.find("\"batch_conversion_ai_cleanup\":false"), std::string::npos) << text;
    // A field the logger redacted would read "<redacted"; these are plain bools.
    EXPECT_NE(text.find("\"english_supported\":false"), std::string::npos) << text;
    ExpectNoToken(text);
  }
}

}  // namespace
