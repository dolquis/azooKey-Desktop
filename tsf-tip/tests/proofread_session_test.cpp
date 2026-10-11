#include <Windows.h>
#include <aclapi.h>
#include <gtest/gtest.h>
#include <sddl.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ProofreadPipeClient.h"
#include "azookey/ipc/Limits.h"
#include "azookey/ipc/ProofreadSession.h"
#include "azookey/tsf/ProofreadSessionServer.h"
#include "azookey/tsf/SettingsLauncher.h"

// Batch correction session (docs/rich-features-spec.md X-3-6): the protocol,
// the session pipe's security, and the TIP server's request cycle.

namespace {

namespace proofread = azookey::ipc::proofread;
using azookey::tsf::ProofreadSessionServer;
using azookey::tsf::test::ProofreadPipeClient;
using azookey::tsf::test::PumpUntil;

constexpr char kToken[] = "0123456789abcdef0123456789abcdef";

// A fresh pipe name per use: CTest runs test processes in parallel.
std::string FreshToken() {
  auto token = proofread::GenerateSessionToken();
  return token ? *token : std::string();
}

TEST(ProofreadProtocolTest, DocumentMessageRoundTrips) {
  proofread::DocumentMessage message;
  message.status = proofread::DocumentStatus::Captured;
  message.text = "校正する本文";
  message.secure = false;
  message.learning_allowed = true;
  message.window = 0x1234;
  const auto parsed = proofread::ParseDocumentMessage(proofread::BuildDocumentMessage(message));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->status, proofread::DocumentStatus::Captured);
  EXPECT_EQ(parsed->text, message.text);
  EXPECT_FALSE(parsed->secure);
  EXPECT_TRUE(parsed->learning_allowed);
  EXPECT_EQ(parsed->window, 0x1234u);
}

TEST(ProofreadProtocolTest, BlockedDocumentNeverCarriesText) {
  proofread::DocumentMessage message;
  message.status = proofread::DocumentStatus::Blocked;
  message.text = "送ってはいけない本文";
  const auto json = proofread::BuildDocumentMessage(message);
  EXPECT_EQ(json.find("送ってはいけない"), std::string::npos);
  const auto parsed = proofread::ParseDocumentMessage(json);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->status, proofread::DocumentStatus::Blocked);
  EXPECT_TRUE(parsed->text.empty());
}

TEST(ProofreadProtocolTest, RejectsDocumentsOutsideTheContract) {
  for (const char* json : {
           R"({"kind":"document","status":"captured","text":""})",
           R"({"kind":"document","status":"blocked","text":"x"})",
           R"({"kind":"document","status":"unknown","text":""})",
           R"({"kind":"select","status":"captured","text":"x"})",
           R"({"kind":"document","status":"captured"})",
           R"(["document"])",
       }) {
    SCOPED_TRACE(json);
    EXPECT_FALSE(proofread::ParseDocumentMessage(json));
  }
  proofread::DocumentMessage oversized;
  oversized.status = proofread::DocumentStatus::Captured;
  oversized.text.assign(azookey::ipc::kMaxAnomalyTextBytes + 1, 'a');
  EXPECT_FALSE(proofread::ParseDocumentMessage(proofread::BuildDocumentMessage(oversized)));
}

TEST(ProofreadProtocolTest, MissingPrivacyReadsAsTheMostRestrictive) {
  const auto parsed =
      proofread::ParseDocumentMessage(R"({"kind":"document","status":"unavailable","text":""})");
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->secure);
  EXPECT_FALSE(parsed->learning_allowed);
}

TEST(ProofreadProtocolTest, SelectRequestAndResponseRoundTrip) {
  const auto request = proofread::ParseSelectRequest(proofread::BuildSelectRequest({12, 3}));
  ASSERT_TRUE(request);
  EXPECT_EQ(request->start, 12u);
  EXPECT_EQ(request->length, 3u);
  EXPECT_FALSE(proofread::ParseSelectRequest(R"({"kind":"select","start":-1,"length":1})"));
  EXPECT_FALSE(proofread::ParseSelectRequest(R"({"kind":"select","start":1})"));
  for (const auto result :
       {proofread::SelectResult::Ok, proofread::SelectResult::DocumentChanged,
        proofread::SelectResult::ContextGone, proofread::SelectResult::InvalidRange,
        proofread::SelectResult::Failed}) {
    EXPECT_EQ(proofread::ParseSelectResponse(proofread::BuildSelectResponse(result)), result);
  }
  EXPECT_FALSE(proofread::ParseSelectResponse(R"({"kind":"select_result","result":"maybe"})"));
}

TEST(ProofreadProtocolTest, FramesAreBoundedAndLengthPrefixed) {
  const auto frame = proofread::EncodeFrame("{}", 16);
  ASSERT_TRUE(frame);
  ASSERT_EQ(frame->size(), 6u);
  const uint8_t header[4]{(*frame)[0], (*frame)[1], (*frame)[2], (*frame)[3]};
  EXPECT_EQ(proofread::DecodeFrameLength(header, 16), 2u);
  EXPECT_FALSE(proofread::DecodeFrameLength(header, 1));
  const uint8_t zero[4]{};
  EXPECT_FALSE(proofread::DecodeFrameLength(zero, 16));
  EXPECT_FALSE(proofread::EncodeFrame("", 16));
  EXPECT_FALSE(proofread::EncodeFrame(std::string(17, 'a'), 16));
}

TEST(ProofreadProtocolTest, TokensAre128RandomBitsInLowercaseHex) {
  const auto first = proofread::GenerateSessionToken();
  const auto second = proofread::GenerateSessionToken();
  ASSERT_TRUE(first && second);
  EXPECT_TRUE(proofread::IsValidSessionToken(*first));
  EXPECT_NE(*first, *second);
  EXPECT_TRUE(proofread::IsValidSessionToken(kToken));
  EXPECT_FALSE(proofread::IsValidSessionToken("0123456789ABCDEF0123456789abcdef"));
  EXPECT_FALSE(proofread::IsValidSessionToken("0123456789abcdef"));
  EXPECT_FALSE(proofread::IsValidSessionToken("0123456789abcdef0123456789abcdeg"));
  EXPECT_FALSE(proofread::IsValidSessionToken("..\\..\\0123456789abcdef0123456789"));
  EXPECT_EQ(proofread::SessionPipeName(kToken),
            L"\\\\.\\pipe\\azookey-proofread-0123456789abcdef0123456789abcdef");
}

std::wstring CurrentLogonSidForTest() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
  DWORD size = 0;
  GetTokenInformation(token, TokenGroups, nullptr, 0, &size);
  std::vector<uint8_t> buffer(size);
  const bool ok = GetTokenInformation(token, TokenGroups, buffer.data(), size, &size);
  CloseHandle(token);
  if (!ok) return {};
  const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(buffer.data());
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID) continue;
    wchar_t* sid = nullptr;
    if (!ConvertSidToStringSidW(groups->Groups[i].Sid, &sid)) return {};
    std::wstring result(sid);
    LocalFree(sid);
    return result;
  }
  return {};
}

TEST(ProofreadSessionPipeTest, AdmitsOnlyTheLogonSessionAndOneInstance) {
  const auto token = proofread::GenerateSessionToken();
  ASSERT_TRUE(token);
  const HANDLE pipe = proofread::CreateSessionPipe(*token);
  ASSERT_NE(pipe, INVALID_HANDLE_VALUE);

  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  ASSERT_EQ(GetSecurityInfo(pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                            &dacl, nullptr, &descriptor),
            static_cast<DWORD>(ERROR_SUCCESS));
  ASSERT_NE(dacl, nullptr);
  EXPECT_EQ(dacl->AceCount, 1u);
  void* ace = nullptr;
  ASSERT_TRUE(GetAce(dacl, 0, &ace));
  const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(ace);
  EXPECT_EQ(allowed->Header.AceType, ACCESS_ALLOWED_ACE_TYPE);
  wchar_t* sid = nullptr;
  ASSERT_TRUE(ConvertSidToStringSidW(const_cast<DWORD*>(&allowed->SidStart), &sid));
  EXPECT_EQ(std::wstring(sid), CurrentLogonSidForTest());
  LocalFree(sid);
  LocalFree(descriptor);

  DWORD flags = 0;
  DWORD max_instances = 0;
  ASSERT_TRUE(GetNamedPipeInfo(pipe, &flags, nullptr, nullptr, &max_instances));
  EXPECT_EQ(max_instances, 1u);
  // FILE_FLAG_FIRST_PIPE_INSTANCE: no second server can take the name.
  EXPECT_EQ(proofread::CreateSessionPipe(*token), INVALID_HANDLE_VALUE);
  CloseHandle(pipe);
}

TEST(ProofreadSessionPipeTest, RefusesAnInvalidToken) {
  EXPECT_EQ(proofread::CreateSessionPipe("not-a-token"), INVALID_HANDLE_VALUE);
}

TEST(ProofreadProcessTest, RunsOnlyInAMediumUnelevatedDesktopProcess) {
  using azookey::tsf::ProofreadAllowedForToken;
  EXPECT_TRUE(ProofreadAllowedForToken({false, false, SECURITY_MANDATORY_MEDIUM_RID}));
  EXPECT_TRUE(ProofreadAllowedForToken({false, false, SECURITY_MANDATORY_MEDIUM_PLUS_RID}));
  EXPECT_FALSE(ProofreadAllowedForToken({false, true, SECURITY_MANDATORY_MEDIUM_RID}));
  EXPECT_FALSE(ProofreadAllowedForToken({false, false, SECURITY_MANDATORY_HIGH_RID}));
  EXPECT_FALSE(ProofreadAllowedForToken({false, false, SECURITY_MANDATORY_LOW_RID}));
  EXPECT_FALSE(ProofreadAllowedForToken({true, false, SECURITY_MANDATORY_LOW_RID}));
  EXPECT_FALSE(ProofreadAllowedForToken({true, false, SECURITY_MANDATORY_MEDIUM_RID}));
  EXPECT_TRUE(azookey::tsf::QueryCurrentProcessTokenForProofread().has_value());
}

TEST(ProofreadSelectionTest, ChecksBoundsAndSurrogatePairs) {
  using azookey::tsf::IsValidProofreadSelection;
  const std::wstring text = L"a\U0001F600b";  // a, D83D, DE00, b
  EXPECT_TRUE(IsValidProofreadSelection(text, 0, 4));
  EXPECT_TRUE(IsValidProofreadSelection(text, 1, 2));
  EXPECT_TRUE(IsValidProofreadSelection(text, 4, 0));
  EXPECT_FALSE(IsValidProofreadSelection(text, 2, 1));
  EXPECT_FALSE(IsValidProofreadSelection(text, 1, 1));
  EXPECT_FALSE(IsValidProofreadSelection(text, 3, 2));
  EXPECT_FALSE(IsValidProofreadSelection(text, 5, 0));
  EXPECT_FALSE(IsValidProofreadSelection(text, 1, UINT64_MAX));
}

TEST(ProofreadSelectionTest, HashTellsChangedTextApart) {
  using azookey::tsf::ProofreadTextHash;
  EXPECT_EQ(ProofreadTextHash(L"本文"), ProofreadTextHash(L"本文"));
  EXPECT_NE(ProofreadTextHash(L"本文"), ProofreadTextHash(L"本分"));
  EXPECT_NE(ProofreadTextHash(L"ab"), ProofreadTextHash(L"ba"));
}

struct ServerProbe {
  std::vector<proofread::SelectRequest> requests;
  int ends{0};
  ProofreadSessionServer* server{nullptr};
  std::optional<proofread::SelectResult> answer;
  std::optional<bool> held_document_at_select;
};

void ProbeSelect(void* context, proofread::SelectRequest request) noexcept {
  auto* probe = static_cast<ServerProbe*>(context);
  probe->requests.push_back(request);
  if (probe->server) probe->held_document_at_select = probe->server->holds_document_for_test();
  if (probe->answer) probe->server->CompleteSelection(*probe->answer);
}

void ProbeEnd(void* context) noexcept { ++static_cast<ServerProbe*>(context)->ends; }

bool AcceptAnyClient(HANDLE) noexcept { return true; }
bool RefuseEveryClient(HANDLE) noexcept { return false; }

ProofreadSessionServer::Options TestOptions(ProofreadSessionServer::ClientVerifier verifier) {
  ProofreadSessionServer::Options options;
  options.verifier = verifier;
  return options;
}

std::string TestDocument() {
  proofread::DocumentMessage message;
  message.status = proofread::DocumentStatus::Captured;
  message.text = "本文";
  message.secure = false;
  message.learning_allowed = true;
  return proofread::BuildDocumentMessage(message);
}

TEST(ProofreadSessionServerTest, ServesTheDocumentAndAnswersSelectRequests) {
  const auto token = FreshToken();
  ServerProbe probe;
  ProofreadSessionServer server;
  probe.server = &server;
  probe.answer = proofread::SelectResult::Ok;
  ASSERT_TRUE(server.Start(token, TestDocument(), {&probe, &ProbeSelect, &ProbeEnd},
                           TestOptions(&AcceptAnyClient)));

  std::atomic<bool> finished{false};
  std::optional<std::string> document;
  std::optional<std::string> response;
  std::thread client_thread([&] {
    ProofreadPipeClient client;
    if (client.Connect(token)) {
      document = client.Read(proofread::kMaxDocumentFrameBytes);
      if (client.Write(proofread::BuildSelectRequest({1, 1})))
        response = client.Read(proofread::kMaxRequestFrameBytes);
    }
    finished = true;
  });
  const bool pumped = PumpUntil([&] { return finished.load() && probe.ends > 0; });
  server.Stop();
  client_thread.join();
  ASSERT_TRUE(pumped);

  ASSERT_TRUE(document);
  const auto parsed = proofread::ParseDocumentMessage(*document);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->text, "本文");
  ASSERT_EQ(probe.requests.size(), 1u);
  // The text left the server's memory once the document was sent.
  EXPECT_EQ(probe.held_document_at_select, false);
  EXPECT_EQ(probe.requests[0].start, 1u);
  EXPECT_EQ(probe.requests[0].length, 1u);
  ASSERT_TRUE(response);
  EXPECT_EQ(proofread::ParseSelectResponse(*response), proofread::SelectResult::Ok);
  // The client closed after its answer; the session ended on its own.
  EXPECT_EQ(probe.ends, 1);
}

TEST(ProofreadSessionServerTest, RefusedClientNeverReceivesTheDocument) {
  const auto token = FreshToken();
  ServerProbe probe;
  ProofreadSessionServer server;
  probe.server = &server;
  auto options = TestOptions(&RefuseEveryClient);
  options.max_rejected_clients = 1;
  ASSERT_TRUE(server.Start(token, TestDocument(), {&probe, &ProbeSelect, &ProbeEnd}, options));

  std::atomic<bool> finished{false};
  bool connected = false;
  std::optional<std::string> document;
  std::thread client_thread([&] {
    ProofreadPipeClient client;
    connected = client.Connect(token);
    if (connected) document = client.Read(proofread::kMaxDocumentFrameBytes);
    finished = true;
  });
  const bool pumped = PumpUntil([&] { return finished.load() && probe.ends > 0; });
  server.Stop();
  client_thread.join();
  ASSERT_TRUE(pumped);
  EXPECT_TRUE(connected);
  EXPECT_FALSE(document);
  EXPECT_TRUE(probe.requests.empty());
}

TEST(ProofreadSessionServerTest, EndsAfterTheIdleLimitWithoutAClient) {
  const auto token = FreshToken();
  ServerProbe probe;
  ProofreadSessionServer server;
  auto options = TestOptions(&AcceptAnyClient);
  options.idle_timeout = std::chrono::milliseconds(20);
  ASSERT_TRUE(server.Start(token, TestDocument(), {&probe, &ProbeSelect, &ProbeEnd}, options));
  EXPECT_TRUE(PumpUntil([&] { return probe.ends > 0; }));
  EXPECT_FALSE(server.active());
}

TEST(ProofreadSessionServerTest, EndsOnAnOversizedOrMalformedRequest) {
  const auto token = FreshToken();
  for (const bool oversized : {true, false}) {
    SCOPED_TRACE(oversized ? "oversized" : "malformed");
    ServerProbe probe;
    ProofreadSessionServer server;
    probe.server = &server;
    ASSERT_TRUE(server.Start(token, TestDocument(), {&probe, &ProbeSelect, &ProbeEnd},
                             TestOptions(&AcceptAnyClient)));
    std::atomic<bool> finished{false};
    std::thread client_thread([&] {
      ProofreadPipeClient client;
      if (client.Connect(token) && client.Read(proofread::kMaxDocumentFrameBytes)) {
        if (oversized) {
          const uint32_t size = proofread::kMaxRequestFrameBytes + 1;
          const uint8_t header[4]{static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8),
                                  static_cast<uint8_t>(size >> 16),
                                  static_cast<uint8_t>(size >> 24)};
          client.WriteRaw(header, sizeof(header));
        } else {
          client.Write(R"({"kind":"select","start":"x"})");
        }
        // The server closes instead of answering.
        EXPECT_FALSE(client.Read(proofread::kMaxRequestFrameBytes));
      }
      finished = true;
    });
    const bool pumped = PumpUntil([&] { return finished.load() && probe.ends > 0; });
    server.Stop();
    client_thread.join();
    EXPECT_TRUE(pumped);
    EXPECT_TRUE(probe.requests.empty());
  }
}

TEST(ProofreadSessionServerTest, StopRunsNoCallback) {
  const auto token = FreshToken();
  ServerProbe probe;
  ProofreadSessionServer server;
  ASSERT_TRUE(server.Start(token, TestDocument(), {&probe, &ProbeSelect, &ProbeEnd},
                           TestOptions(&AcceptAnyClient)));
  EXPECT_TRUE(server.active());
  server.Stop();
  EXPECT_FALSE(server.active());
  EXPECT_EQ(probe.ends, 0);
  // The name is free again once the session stopped.
  ASSERT_TRUE(server.Start(token, TestDocument(), {&probe, &ProbeSelect, &ProbeEnd},
                           TestOptions(&AcceptAnyClient)));
  server.Stop();
}

struct SettingsLauncherReset {
  ~SettingsLauncherReset() { azookey::tsf::testing::ClearSettingsLauncherForTest(); }
};

std::wstring CurrentExecutablePath() {
  std::wstring path(32768, L'\0');
  path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
  return path;
}

bool VerifyConnectedClient() {
  const auto token = FreshToken();
  const HANDLE pipe = proofread::CreateSessionPipe(token);
  if (pipe == INVALID_HANDLE_VALUE) return false;
  ProofreadPipeClient client;
  const bool verified =
      client.Connect(token) && ProofreadSessionServer::VerifySettingsAppClient(pipe);
  client.Close();
  CloseHandle(pipe);
  return verified;
}

// The default verifier admits only the image the TIP launches as the
// settings app; this test process stands in for it.
TEST(ProofreadSessionServerTest, DefaultVerifierAdmitsOnlyTheSettingsAppImage) {
  const auto token = FreshToken();
  SettingsLauncherReset reset;
  azookey::tsf::testing::SetSettingsLauncherForTest(CurrentExecutablePath(), nullptr);
  EXPECT_TRUE(VerifyConnectedClient());
  azookey::tsf::testing::SetSettingsLauncherForTest(CurrentExecutablePath() + L".other.exe",
                                                    nullptr);
  EXPECT_FALSE(VerifyConnectedClient());
}

}  // namespace
