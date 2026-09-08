#include <gtest/gtest.h>

#include <tuple>

#include "azookey/core/AiPrivacy.h"
#include "azookey/host/AiBackend.h"
#include "azookey/ipc/Json.h"

namespace {
using namespace azookey::host;
namespace j = azookey::ipc::json;
AiTransformRequest Cleanup() {
  AiTransformRequest request;
  request.task = AiTask::Cleanup;
  request.text = "きょうはいえてんき";
  request.raw_romaji = "kyouha ietenki";
  request.ai_allowed = request.external_allowed = true;
  return request;
}
AiBackendOptions Remote() {
  AiBackendOptions options;
  options.backend = "openai";
  options.api_key = "test-only-placeholder";
  return options;
}
std::string Reply(std::string content) {
  return j::Stringify(j::Object{
      {"choices", j::Array{j::Object{{"message", j::Object{{"content", std::move(content)}}}}}}});
}
TEST(AiBackendTest, CleanupCarriesRawInputAndHonorsContextPolicy) {
  std::string body;
  AiBackend backend([&](const auto&, const std::string& value, const auto*, auto) {
    body = value;
    return AiHttpResponse{200, Reply(R"({"result":"今日はいい天気"})")};
  });
  auto request = Cleanup();
  request.left_context = "private context";
  auto options = Remote();
  options.include_context = false;
  const auto result = backend.Transform(request, options);
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.result, "今日はいい天気");
  const auto parsed = j::Parse(body);
  ASSERT_TRUE(parsed);
  const auto* messages = parsed->GetArray("messages");
  ASSERT_TRUE(messages);
  const auto input = j::Parse(messages->at(1).GetString("content").value());
  ASSERT_TRUE(input);
  EXPECT_EQ(input->GetString("raw_romaji"), request.raw_romaji);
  EXPECT_EQ(input->GetString("left_context"), "");
  EXPECT_NE(messages->at(0).GetString("content")->find("Do not insert punctuation"),
            std::string::npos);
  request.auto_punctuation = true;
  EXPECT_TRUE(backend.Transform(request, options).ok);
  EXPECT_NE(body.find("Insert appropriate Japanese punctuation"), std::string::npos);
}
TEST(AiBackendTest, RetryAfterBeyondBudgetPreservesFailure) {
  for (auto [status, expected] :
       {std::pair{429u, AiErrorClass::RateLimit}, std::pair{503u, AiErrorClass::ServerError}}) {
    unsigned calls = 0;
    AiBackend backend([&](const auto&, const auto&, const auto*, auto) {
      ++calls;
      return AiHttpResponse{status, {}, AiErrorClass::None, 60};
    });
    EXPECT_EQ(backend.Transform(Cleanup(), Remote()).error_class, expected);
    EXPECT_EQ(calls, 1u);
  }
}
TEST(AiBackendTest, LocalDeadlineDoesNotUseExternalTimeout) {
  auto options = Remote();
  options.backend = "local-zenzai";
  options.timeout_ms = 120000;
  AiBackend backend;
  const auto result =
      backend.Transform(Cleanup(), options, nullptr, [&](const auto&, const auto*, auto deadline) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        EXPECT_LE(remaining, std::chrono::seconds(30));
        EXPECT_GT(remaining, std::chrono::seconds(29));
        return AiTransformResult{true, "整文", AiErrorClass::None};
      });
  EXPECT_TRUE(result.ok);
}
TEST(AiBackendTest, RejectsInvalidUtf8AndNulInOutput) {
  for (auto output : {std::string("\xFF"), std::string("a\0b", 3)}) {
    AiBackend backend([&](const auto&, const auto&, const auto*, auto) {
      return AiHttpResponse{200, Reply(output)};
    });
    EXPECT_FALSE(backend.Transform(Cleanup(), Remote()).ok);
  }
}
TEST(AiBackendTest, SecureAndDisabledNeverReachEitherBackend) {
  unsigned calls = 0;
  AiBackend backend([&](const auto&, const auto&, const auto*, auto) {
    ++calls;
    return AiHttpResponse{};
  });
  AiLocalTransform local = [&](const auto&, const auto*, auto) {
    ++calls;
    return AiTransformResult{};
  };
  auto request = Cleanup();
  request.ai_allowed = false;
  EXPECT_EQ(backend.Transform(request, Remote(), nullptr, local).error_class,
            AiErrorClass::BlockedBySecure);
  request.ai_allowed = true;
  EXPECT_EQ(backend.Transform(request, {}).error_class, AiErrorClass::Disabled);
  EXPECT_EQ(calls, 0u);
}
TEST(AiBackendTest, PrivateAndAutomaticLintNeverUseRemote) {
  unsigned remote = 0, local = 0;
  AiBackend backend([&](const auto&, const auto&, const auto*, auto) {
    ++remote;
    return AiHttpResponse{};
  });
  AiLocalTransform callback = [&](const auto&, const auto*, auto) {
    ++local;
    return AiTransformResult{};
  };
  auto request = Cleanup();
  request.external_allowed = false;
  backend.Transform(request, Remote(), nullptr, callback);
  request.external_allowed = true;
  request.task = AiTask::Lint;
  backend.Transform(request, Remote(), nullptr, callback);
  EXPECT_EQ(remote, 0u);
  EXPECT_EQ(local, 2u);
}
TEST(AiBackendTest, ClassifiesFailuresAndBoundsRetries) {
  for (const auto [status, expected, attempts] : {std::tuple{401u, AiErrorClass::Auth, 1u},
                                                  {429u, AiErrorClass::RateLimit, 3u},
                                                  {500u, AiErrorClass::ServerError, 3u},
                                                  {400u, AiErrorClass::Parse, 1u}}) {
    unsigned calls = 0;
    AiBackend backend([&](const auto&, const auto&, const auto*, auto) {
      ++calls;
      return AiHttpResponse{status};
    });
    EXPECT_EQ(backend.Transform(Cleanup(), Remote()).error_class, expected);
    EXPECT_EQ(calls, attempts);
  }
}
TEST(AiBackendTest, CancelStopsRetryAndDoesNotPublishLateSuccess) {
  std::atomic<bool> cancel{false};
  unsigned calls = 0;
  AiBackend backend([&](const auto&, const auto&, const auto*, auto) {
    ++calls;
    cancel = true;
    return AiHttpResponse{200, Reply("late result")};
  });
  EXPECT_EQ(backend.Transform(Cleanup(), Remote(), &cancel).error_class, AiErrorClass::Canceled);
  EXPECT_EQ(calls, 1u);
}
TEST(AiBackendTest, RejectsEmptyMalformedAndOversizedResponses) {
  for (const auto& body : {std::string("invalid"), Reply(""), Reply(R"({"unexpected":true})"),
                           Reply(std::string(65537, 'a'))}) {
    AiBackend backend(
        [&](const auto&, const auto&, const auto*, auto) { return AiHttpResponse{200, body}; });
    EXPECT_EQ(backend.Transform(Cleanup(), Remote()).error_class, AiErrorClass::Parse);
  }
}
TEST(AiBackendTest, PunctuationOffPreservesOnlyExistingMarks) {
  AiBackend backend([](const auto&, const auto&, const auto*, auto) {
    return AiHttpResponse{200, Reply("今日は、晴れ。明日も、晴れ！")};
  });
  auto request = Cleanup();
  request.text = "きょうは、はれあしたもはれ";
  EXPECT_EQ(backend.Transform(request, Remote()).result, "今日は、晴れ明日も晴れ");
  request.auto_punctuation = true;
  EXPECT_EQ(backend.Transform(request, Remote()).result, "今日は、晴れ。明日も、晴れ！");
}

TEST(AiPrivacyTest, DefaultsAndCustomIntersection) {
  const auto normal = azookey::core::ParseAiPrivacy(j::Object{});
  EXPECT_TRUE(normal.ai);
  EXPECT_TRUE(normal.external);
  for (const auto* mode : {"secure", "unknown"}) {
    auto p = azookey::core::ParseAiPrivacy(j::Object{{"privacy", j::Object{{"mode", mode}}}});
    EXPECT_FALSE(p.ai);
    EXPECT_FALSE(p.external);
  }
  const auto p = azookey::core::ParseAiPrivacy(j::Object{
      {"privacy", j::Object{{"mode", "custom"},
                            {"custom", j::Object{{"aiCandidate", false}, {"externalAi", true}}}}}});
  EXPECT_FALSE(p.ai);
  EXPECT_FALSE(p.external);
}
}  // namespace
