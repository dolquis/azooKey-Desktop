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

// An obvious dummy in the token format; real tokens come from GenerateSessionToken.
constexpr char kToken[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

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
  EXPECT_FALSE(request->reconvert);
  const auto reconvert =
      proofread::ParseSelectRequest(proofread::BuildSelectRequest({12, 3, true}));
  ASSERT_TRUE(reconvert);
  EXPECT_TRUE(reconvert->reconvert);
  // A settings app that does not send the flag asks for the selection only.
  const auto legacy = proofread::ParseSelectRequest(R"({"kind":"select","start":1,"length":2})");
  ASSERT_TRUE(legacy);
  EXPECT_FALSE(legacy->reconvert);
  // A flag that is not a bool reads as false and keeps the request.
  for (const char* json : {R"({"kind":"select","start":1,"length":2,"reconvert":"yes"})",
                           R"({"kind":"select","start":1,"length":2,"reconvert":1})"}) {
    SCOPED_TRACE(json);
    const auto odd = proofread::ParseSelectRequest(json);
    ASSERT_TRUE(odd);
    EXPECT_FALSE(odd->reconvert);
  }
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

// DEV-1583: a finding's suggestions ride on select as text candidates.
TEST(ProofreadProtocolTest, SelectRequestCarriesSuggestions) {
  proofread::SelectRequest sent{3, 2, true, {"明日", "あした"}};
  const auto parsed = proofread::ParseSelectRequest(proofread::BuildSelectRequest(sent));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->suggestions, sent.suggestions);
  // None is not sent, and a settings app that does not know them sends none.
  const auto none = proofread::BuildSelectRequest({3, 2, true});
  EXPECT_EQ(none.find("suggestions"), std::string::npos);
  const auto legacy = proofread::ParseSelectRequest(R"({"kind":"select","start":1,"length":2})");
  ASSERT_TRUE(legacy);
  EXPECT_TRUE(legacy->suggestions.empty());
}

TEST(ProofreadProtocolTest, SuggestionsAreBoundedPlainText) {
  EXPECT_TRUE(proofread::IsValidSelectSuggestion("明日"));
  EXPECT_TRUE(proofread::IsValidSelectSuggestion("a \"quoted\" \\ path"));
  EXPECT_TRUE(proofread::IsValidSelectSuggestion(std::string(256, 'a')));
  // The longest valid UTF-8 within the byte limit (85 three-byte characters).
  std::string japanese;
  for (int i = 0; i < 85; ++i) japanese += "あ";
  EXPECT_TRUE(proofread::IsValidSelectSuggestion(japanese));
  EXPECT_FALSE(proofread::IsValidSelectSuggestion(japanese + "あ"));
  EXPECT_TRUE(proofread::IsValidSelectSuggestion("\xF0\x9F\x98\x80"));  // U+1F600
  for (const std::string refused : {
           std::string(),
           std::string(257, 'a'),
           std::string("a\tb"),
           std::string("a\nb"),
           std::string("a\rb"),
           std::string("a\x7F"),
           std::string("a\xC2\x85"),  // U+0085
           std::string("a\xD8\x9C"),  // U+061C
           std::string("a\xE2\x80\x8E"),
           std::string("a\xE2\x80\x8F"),  // U+200E/F
           std::string("a\xE2\x80\xA8"),
           std::string("a\xE2\x80\xA9"),  // U+2028/9
           std::string("a\xE2\x80\xAA"),
           std::string("a\xE2\x80\xAE"),  // U+202A, U+202E
           std::string("a\xE2\x81\xA6"),
           std::string("a\xE2\x81\xA9"),       // U+2066, U+2069
           std::string("\xEF\xBB\xBF") + "a",  // U+FEFF
           std::string("a\xEF\xBF\xB9"),
           std::string("a\xEF\xBF\xBB"),  // U+FFF9, U+FFFB
           std::string("a\xFF"),
           std::string("a\xC3"),         // not UTF-8
           std::string("\xC0\xAF"),      // overlong
           std::string("\xED\xA0\x80"),  // a surrogate
           std::string("a\0b", 3),
       }) {
    SCOPED_TRACE(refused);
    EXPECT_FALSE(proofread::IsValidSelectSuggestion(refused));
  }
}

// Invisible characters hide text; ZWNJ / ZWJ and variation selectors stay for emoji and kanji.
TEST(ProofreadProtocolTest, InvisibleCharactersAreRefusedButJoinersAndSelectorsStay) {
  EXPECT_FALSE(proofread::IsValidSelectSuggestion(std::string("a\xE2\x80\x8B")));      // U+200B
  EXPECT_FALSE(proofread::IsValidSelectSuggestion(std::string("a\xE2\x81\xA0")));      // U+2060
  EXPECT_FALSE(proofread::IsValidSelectSuggestion(std::string("a\xE2\x81\xA4")));      // U+2064
  EXPECT_FALSE(proofread::IsValidSelectSuggestion(std::string("a\xF3\xA0\x80\x80")));  // U+E0000
  EXPECT_FALSE(proofread::IsValidSelectSuggestion(std::string("a\xF3\xA0\x81\x81")));  // U+E0041
  EXPECT_FALSE(proofread::IsValidSelectSuggestion(std::string("a\xF3\xA0\x81\xBF")));  // U+E007F
  EXPECT_TRUE(proofread::IsValidSelectSuggestion(
      std::string("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9")));  // a ZWJ emoji sequence
  EXPECT_TRUE(proofread::IsValidSelectSuggestion(std::string("a\xE2\x80\x8C")));  // ZWNJ
  EXPECT_TRUE(proofread::IsValidSelectSuggestion(
      std::string("\xE8\x91\x9B\xF3\xA0\x84\x80")));  // U+845B with U+E0100
  EXPECT_TRUE(proofread::IsValidSelectSuggestion(
      std::string("\xE2\x9D\xA4\xEF\xB8\x8F")));  // U+2764 with U+FE0F
  // The same through JSON escapes (U+E0041 and U+E0100 as surrogate pairs).
  const auto parsed = proofread::ParseSelectRequest(
      "{\"kind\":\"select\",\"start\":1,\"length\":2,\"suggestions\":["
      "\"a\\u200bb\",\"a\\u2060b\",\"a\\u2064b\",\"a\\udb40\\udc41b\",\"a\\u200db\","
      "\"\\u845b\\udb40\\udd00\",\"ok\"]}");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->suggestions,
            (std::vector<std::string>{std::string("a\xE2\x80\x8D") + "b",
                                      std::string("\xE8\x91\x9B\xF3\xA0\x84\x80"), "ok"}));
}

TEST(ProofreadProtocolTest, MalformedSuggestionsAreDroppedOneByOne) {
  // Escapes decode before the check, so they are refused like the raw characters. The JSON
  // escapes are in ordinary literals: some MSVC versions expand universal character names inside
  // raw string literals.
  const auto parsed = proofread::ParseSelectRequest(
      R"({"kind":"select","start":1,"length":2,"reconvert":true,"suggestions":)"
      "[\"ok\",1,\"\",null,[\"x\"],\"a\\nb\",\"a\\u202eb\",\"a\\u2028b\",\"\\ufeffa\","
      "\"a\\u0085b\",\"a\\u2066b\",\"good\"]}");
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->reconvert);
  EXPECT_EQ(parsed->suggestions, (std::vector<std::string>{"ok", "good"}));
  const auto long_element =
      proofread::ParseSelectRequest(R"({"kind":"select","start":1,"length":2,"suggestions":[")" +
                                    std::string(257, 'a') + R"(","b"]})");
  ASSERT_TRUE(long_element);
  EXPECT_EQ(long_element->suggestions, (std::vector<std::string>{"b"}));
  // Only the first kMaxSelectSuggestions valid ones are kept.
  const auto many = proofread::ParseSelectRequest(
      R"({"kind":"select","start":1,"length":2,"suggestions":["1","2","3","4","5","6"]})");
  ASSERT_TRUE(many);
  EXPECT_EQ(many->suggestions, (std::vector<std::string>{"1", "2", "3", "4", "5"}));
  // A string JSON cannot hold (an unpaired surrogate) makes the whole request unreadable, like
  // any other malformed message.
  EXPECT_FALSE(proofread::ParseSelectRequest(
      "{\"kind\":\"select\",\"start\":1,\"length\":2,\"suggestions\":[\"a\\ud800b\"]}"));
  // Suggestions that are not an array read as none and keep the request.
  for (const char* json : {R"({"kind":"select","start":1,"length":2,"suggestions":"x"})",
                           R"({"kind":"select","start":1,"length":2,"suggestions":{"a":"x"}})",
                           R"({"kind":"select","start":1,"length":2,"suggestions":null})"}) {
    SCOPED_TRACE(json);
    const auto odd = proofread::ParseSelectRequest(json);
    ASSERT_TRUE(odd);
    EXPECT_TRUE(odd->suggestions.empty());
  }
  // The builder drops the same elements, so a settings app never sends them.
  proofread::SelectRequest dirty{1, 2, true, {"", "a\nb", std::string(257, 'a'), "1", "2"}};
  for (int i = 3; i <= 7; ++i) dirty.suggestions.push_back(std::to_string(i));
  const auto rebuilt = proofread::ParseSelectRequest(proofread::BuildSelectRequest(dirty));
  ASSERT_TRUE(rebuilt);
  EXPECT_EQ(rebuilt->suggestions, (std::vector<std::string>{"1", "2", "3", "4", "5"}));
}

TEST(ProofreadProtocolTest, LargestSelectRequestFitsTheRequestFrame) {
  // Every byte escaped: the most the builder can produce.
  proofread::SelectRequest request{UINT64_MAX, UINT64_MAX, true, {}};
  for (std::size_t i = 0; i < proofread::kMaxSelectSuggestions; ++i)
    request.suggestions.push_back(std::string(proofread::kMaxSelectSuggestionBytes, '"'));
  const auto json = proofread::BuildSelectRequest(request);
  EXPECT_TRUE(proofread::EncodeFrame(json, proofread::kMaxRequestFrameBytes));
  const auto parsed = proofread::ParseSelectRequest(json);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->suggestions, request.suggestions);
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
  EXPECT_FALSE(proofread::IsValidSessionToken("AAAAAAAAAAAAAAAAaaaaaaaaaaaaaaaa"));
  EXPECT_FALSE(proofread::IsValidSessionToken("aaaaaaaaaaaaaaaa"));
  EXPECT_FALSE(proofread::IsValidSessionToken("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag"));
  EXPECT_FALSE(proofread::IsValidSessionToken("..\\..\\aaaaaaaaaaaaaaaaaaaaaaaaaa"));
  EXPECT_EQ(proofread::SessionPipeName(kToken),
            L"\\\\.\\pipe\\azookey-proofread-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
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

  // A medium mandatory label with no-read-up / no-write-up / no-execute-up.
  PACL sacl = nullptr;
  PSECURITY_DESCRIPTOR label_descriptor = nullptr;
  ASSERT_EQ(GetSecurityInfo(pipe, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr,
                            nullptr, &sacl, &label_descriptor),
            static_cast<DWORD>(ERROR_SUCCESS));
  ASSERT_NE(sacl, nullptr);
  ASSERT_EQ(sacl->AceCount, 1u);
  void* label_ace = nullptr;
  ASSERT_TRUE(GetAce(sacl, 0, &label_ace));
  const auto* label = static_cast<const SYSTEM_MANDATORY_LABEL_ACE*>(label_ace);
  EXPECT_EQ(label->Header.AceType, SYSTEM_MANDATORY_LABEL_ACE_TYPE);
  EXPECT_EQ(label->Mask, static_cast<ACCESS_MASK>(SYSTEM_MANDATORY_LABEL_NO_READ_UP |
                                                  SYSTEM_MANDATORY_LABEL_NO_WRITE_UP |
                                                  SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP));
  const auto label_sid = const_cast<DWORD*>(&label->SidStart);
  EXPECT_EQ(*GetSidSubAuthority(label_sid, *GetSidSubAuthorityCount(label_sid) - 1u),
            static_cast<DWORD>(SECURITY_MANDATORY_MEDIUM_RID));
  LocalFree(label_descriptor);

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
