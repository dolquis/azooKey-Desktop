#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

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
// DEV-1532: anomaly detection is remote-only and carries the persona hint.
TEST(AiBackendTest, AnomaliesNeverUseTheLocalPathAndCarryThePersonaHint) {
  unsigned local_calls = 0;
  AiLocalTransform local = [&](const auto&, const auto*, auto) {
    ++local_calls;
    return AiTransformResult{true, "[]", AiErrorClass::None};
  };
  std::string body;
  AiBackend backend([&](const auto&, const std::string& value, const auto*, auto) {
    body = value;
    return AiHttpResponse{200, Reply(R"({"result":"[]"})")};
  });
  AiTransformRequest request;
  request.task = AiTask::Anomalies;
  request.text = "今日は晴れでした。";
  request.prompt = "The writer's usual style: polite 0.80. ";
  request.ai_allowed = true;
  request.external_allowed = false;
  EXPECT_EQ(backend.Transform(request, Remote(), nullptr, local).error_class,
            AiErrorClass::Disabled);
  auto local_options = Remote();
  local_options.backend = "local-zenzai";
  request.external_allowed = true;
  EXPECT_EQ(backend.Transform(request, local_options, nullptr, local).error_class,
            AiErrorClass::Disabled);
  EXPECT_EQ(local_calls, 0u);
  EXPECT_TRUE(body.empty());

  const auto result = backend.Transform(request, Remote(), nullptr, local);
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.result, "[]");
  EXPECT_NE(body.find("polite 0.80"), std::string::npos);
  EXPECT_NE(body.find("quote"), std::string::npos);
}

// DEV-1963: the selection transform (M16) is remote-only too, and the free
// mode carries the user's instruction.
TEST(AiBackendTest, TransformNeverUsesTheLocalPathAndCarriesTheInstruction) {
  unsigned local_calls = 0;
  AiLocalTransform local = [&](const auto&, const auto*, auto) {
    ++local_calls;
    return AiTransformResult{true, "変換", AiErrorClass::None};
  };
  std::string body;
  AiBackend backend([&](const auto&, const std::string& value, const auto*, auto) {
    body = value;
    return AiHttpResponse{200, Reply(R"({"result":"おはようございます。"})")};
  });
  AiTransformRequest request;
  request.task = AiTask::Transform;
  request.mode = "free";
  request.prompt = "日本語に翻訳";
  request.text = "Good morning.";
  request.ai_allowed = true;
  request.external_allowed = false;
  EXPECT_EQ(backend.Transform(request, Remote(), nullptr, local).error_class,
            AiErrorClass::Disabled);
  auto local_options = Remote();
  local_options.backend = "local-zenzai";
  request.external_allowed = true;
  EXPECT_EQ(backend.Transform(request, local_options, nullptr, local).error_class,
            AiErrorClass::Disabled);
  EXPECT_EQ(local_calls, 0u);
  EXPECT_TRUE(body.empty());

  const auto result = backend.Transform(request, Remote(), nullptr, local);
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.result, "おはようございます。");
  EXPECT_NE(body.find("日本語に翻訳"), std::string::npos);
  EXPECT_NE(body.find("Good morning."), std::string::npos);
  EXPECT_EQ(local_calls, 0u);
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
                                                  {403u, AiErrorClass::Auth, 1u},
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
// ai-backend-spec section 7.3: the selection transform (M16) keeps the user
// waiting, so a retryable failure is retried once instead of twice.
TEST(AiBackendTest, TransformRetriesOnceAtMost) {
  for (const auto [status, expected, attempts] : {std::tuple{401u, AiErrorClass::Auth, 1u},
                                                  {429u, AiErrorClass::RateLimit, 2u},
                                                  {500u, AiErrorClass::ServerError, 2u}}) {
    unsigned calls = 0;
    AiBackend backend([&](const auto&, const auto&, const auto*, auto) {
      ++calls;
      return AiHttpResponse{status};
    });
    AiTransformRequest request;
    request.task = AiTask::Transform;
    request.mode = "free";
    request.prompt = "英語に翻訳";
    request.text = "おはよう";
    request.ai_allowed = request.external_allowed = true;
    EXPECT_EQ(backend.Transform(request, Remote()).error_class, expected);
    EXPECT_EQ(calls, attempts) << status;
  }
}
TEST(AiBackendTest, TimeoutAndKeyReentryAreNotRetried) {
  for (auto error : {AiErrorClass::Timeout, AiErrorClass::KeyReentry}) {
    unsigned calls = 0;
    AiBackend backend([&](const auto&, const auto&, const auto*, auto) {
      ++calls;
      return AiHttpResponse{0, {}, error};
    });
    EXPECT_EQ(backend.Transform(Cleanup(), Remote()).error_class, error);
    EXPECT_EQ(calls, 1u);
  }
}
TEST(AiBackendTest, SchedulerCancelWakesRetryAfterWait) {
  RequestScheduler scheduler;
  auto cancel = scheduler.TrackCancellation(42);
  ASSERT_NE(cancel, nullptr);
  std::atomic<unsigned> calls{0};
  AiBackend backend([&](const auto&, const auto&, const auto*, auto) {
    ++calls;
    return AiHttpResponse{429, {}, AiErrorClass::None, 2};
  });
  AiTransformResult result;
  std::jthread worker([&] { result = backend.Transform(Cleanup(), Remote(), cancel.get()); });
  const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (calls.load() == 0 && std::chrono::steady_clock::now() < entered_deadline)
    std::this_thread::yield();
  ASSERT_EQ(calls.load(), 1u);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto canceled_at = std::chrono::steady_clock::now();
  scheduler.Cancel(42);
  worker.join();
  EXPECT_EQ(result.error_class, AiErrorClass::Canceled);
  EXPECT_EQ(calls.load(), 1u);
  EXPECT_LT(std::chrono::steady_clock::now() - canceled_at, std::chrono::seconds(1));
  scheduler.CompleteRequest(42);
}
TEST(AiBackendTest, CancelStopsRetryAndDoesNotPublishLateSuccess) {
  AiCancellationFlag cancel;
  unsigned calls = 0;
  AiBackend backend([&](const auto&, const auto&, const auto*, auto) {
    ++calls;
    cancel.Cancel();
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
// DEV-1967: compatibility cases for OpenAI-compatible servers (ai-backend-spec
// section 5.3). The cases below do not depend on whether a 400 is re-sent
// without response_format.
AiTransformResult TransformReplying(const std::string& body) {
  AiBackend backend(
      [&](const auto&, const auto&, const auto*, auto) { return AiHttpResponse{200, body}; });
  return backend.Transform(Cleanup(), Remote());
}
std::string ReplyWithFinishReason(const std::string& finish_reason, const std::string& content) {
  return j::Stringify(
      j::Object{{"choices", j::Array{j::Object{{"finish_reason", finish_reason},
                                               {"message", j::Object{{"content", content}}}}}}});
}
TEST(AiBackendTest, AcceptsPlainTextContentFromServersThatIgnoreResponseFormat) {
  const auto result = TransformReplying(Reply("  今日はいい天気\r\n"));
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.result, "今日はいい天気");
}
TEST(AiBackendTest, RejectsJsonLookingContentThatDoesNotParse) {
  for (const auto* content : {R"({"result":"今日は)", "[", "  {oops}", R"([1, 2)"}) {
    EXPECT_EQ(TransformReplying(Reply(content)).error_class, AiErrorClass::Parse) << content;
  }
}
TEST(AiBackendTest, RejectsValidJsonWithoutAStringResult) {
  for (const auto* content : {R"(["a"])", R"({"result":1})", R"({"result":null})",
                              R"({"results":"x"})", R"({"result":["a"]})"}) {
    EXPECT_EQ(TransformReplying(Reply(content)).error_class, AiErrorClass::Parse) << content;
  }
}
// DEV-2163: a JSON scalar is plain text, not structured output; the trimmed
// content is the result as it stands, quotes included.
TEST(AiBackendTest, AcceptsJsonScalarContentAsPlainText) {
  const std::vector<std::pair<std::string, std::string>> cases{
      {"42", "42"}, {" true\n", "true"}, {"null", "null"}, {R"("引用")", R"("引用")"}};
  for (const auto& [content, expected] : cases) {
    const auto result = TransformReplying(Reply(content));
    EXPECT_TRUE(result.ok) << content;
    EXPECT_EQ(result.result, expected) << content;
  }
}
// DEV-2163: a content that is one ``` fence, untagged or tagged json in any
// case, around a JSON object is that object, under the structured-output rules.
TEST(AiBackendTest, UnwrapsOneJsonFenceAroundAnObject) {
  for (const auto* content :
       {"```json\n{\"result\":\"整文\"}\n```", "```\n{\"result\":\"整文\"}\n```",
        "  ```JSON \r\n{\"result\":\"整文\"}\r\n```\n"}) {
    const auto result = TransformReplying(Reply(content));
    EXPECT_TRUE(result.ok) << content;
    EXPECT_EQ(result.result, "整文") << content;
  }
  EXPECT_EQ(TransformReplying(Reply("```json\n{\"result\":1}\n```")).error_class,
            AiErrorClass::Parse);
}
// Any other fenced content stays plain text as it stands.
TEST(AiBackendTest, KeepsOtherFencedContentAsPlainText) {
  for (const auto* content : {"```python\nx = 1\n```", "```json\n今日は\n```",
                              "```json\n[\"a\"]\n```", "```json\n{\"result\":\"整文\"}",
                              "```{\"result\":\"整文\"}```", "```json\n{\"result\":\"整文\"}```"}) {
    const auto result = TransformReplying(Reply(content));
    EXPECT_TRUE(result.ok) << content;
    EXPECT_EQ(result.result, content) << content;
  }
}
TEST(AiBackendTest, OnlyStopOrAbsentFinishReasonIsAccepted) {
  for (const auto* reason : {"length", "content_filter", "tool_calls", "function_call", ""}) {
    EXPECT_EQ(TransformReplying(ReplyWithFinishReason(reason, R"({"result":"整文"})")).error_class,
              AiErrorClass::Parse)
        << reason;
  }
  const auto stop = TransformReplying(ReplyWithFinishReason("stop", R"({"result":"整文"})"));
  EXPECT_TRUE(stop.ok);
  EXPECT_EQ(stop.result, "整文");
  // Some servers send "finish_reason": null on the final chunk of a non-streamed reply.
  const auto null_reason = TransformReplying(
      R"({"choices":[{"finish_reason":null,"message":{"content":"{\"result\":\"整文\"}"}}]})");
  EXPECT_TRUE(null_reason.ok);
  EXPECT_EQ(null_reason.result, "整文");
}
TEST(AiBackendTest, RejectsRepliesWithoutUsableMessageContent) {
  for (const auto* body : {
           R"({"choices":[]})",
           R"({"choices":[{}]})",
           R"({"choices":[{"message":{}}]})",
           R"({"choices":[{"message":{"content":null,"refusal":"no"}}]})",
           R"({"choices":[{"message":{"content":[{"type":"text","text":"x"}]}}]})",
           R"({"choices":[{"message":{"content":{"result":"x"}}}]})",
           R"({"error":{"message":"overloaded"}})",
           R"([])",
           "",
       }) {
    EXPECT_EQ(TransformReplying(body).error_class, AiErrorClass::Parse) << body;
  }
}
// DEV-1967: a 4xx other than 401/403/429 (for example a server that rejects
// response_format with 400, or 404/422 from a gateway) is classified Parse and
// sent exactly once: the request is not re-sent without response_format, and
// the error body is never read (ai-backend-spec sections 5.3 and 7.2).
TEST(AiBackendTest, ClientErrorsAreNotResentWithoutResponseFormat) {
  const std::string error_body =
      R"({"error":{"message":"Invalid parameter: 'response_format' of type 'json_schema' is not supported with this model."}})";
  for (const auto status : {400u, 404u, 405u, 413u, 415u, 422u}) {
    for (const auto task : {AiTask::Cleanup, AiTask::Transform}) {
      std::vector<std::string> bodies;
      AiBackend backend([&](const auto&, const std::string& value, const auto*, auto) {
        bodies.push_back(value);
        return AiHttpResponse{status, error_body};
      });
      auto request = Cleanup();
      request.task = task;
      request.mode = "free";
      request.prompt = "英語に翻訳";
      const auto result = backend.Transform(request, Remote());
      EXPECT_FALSE(result.ok) << status;
      EXPECT_EQ(result.error_class, AiErrorClass::Parse) << status;
      EXPECT_TRUE(result.result.empty()) << status;
      ASSERT_EQ(bodies.size(), 1u) << status;
      EXPECT_NE(bodies.front().find("response_format"), std::string::npos) << status;
    }
  }
}
TEST(AiBackendTest, RequestAlwaysAsksForStrictJsonSchema) {
  std::string body;
  AiBackend backend([&](const auto&, const std::string& value, const auto*, auto) {
    body = value;
    return AiHttpResponse{200, Reply(R"({"result":"整文"})")};
  });
  auto options = Remote();
  options.model = "compat-model";
  ASSERT_TRUE(backend.Transform(Cleanup(), options).ok);
  const auto parsed = j::Parse(body);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->GetString("model"), "compat-model");
  EXPECT_EQ(parsed->GetBool("stream"), false);
  const auto* format = parsed->Find("response_format");
  ASSERT_TRUE(format);
  EXPECT_EQ(format->GetString("type"), "json_schema");
  const auto* json_schema = format->Find("json_schema");
  ASSERT_TRUE(json_schema);
  EXPECT_EQ(json_schema->GetBool("strict"), true);
  const auto* schema = json_schema->Find("schema");
  ASSERT_TRUE(schema);
  EXPECT_EQ(schema->GetBool("additionalProperties"), false);
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
  EXPECT_FALSE(normal.external);  // No externalAiConsent (ai-backend-spec section 14.2).
  for (const auto* mode : {"secure", "unknown"}) {
    auto p = azookey::core::ParseAiPrivacy(
        j::Object{{"privacy", j::Object{{"mode", mode}, {"externalAiConsent", true}}}});
    EXPECT_FALSE(p.ai);
    EXPECT_FALSE(p.external);
  }
  const auto p = azookey::core::ParseAiPrivacy(j::Object{
      {"privacy", j::Object{{"mode", "custom"},
                            {"custom", j::Object{{"aiCandidate", false}, {"externalAi", true}}}}}});
  EXPECT_FALSE(p.ai);
  EXPECT_FALSE(p.external);
}

// ai-backend-spec section 14.2 and privacy-and-secure-input-spec section 5.2:
// external AI is consent AND the mode's own permission; secure stays first.
TEST(AiPrivacyTest, ExternalAiNeedsConsentInEveryMode) {
  const auto parse = [](j::Object privacy) {
    return azookey::core::ParseAiPrivacy(j::Object{{"privacy", std::move(privacy)}});
  };
  auto p = parse(j::Object{{"mode", "normal"}});
  EXPECT_TRUE(p.ai);
  EXPECT_FALSE(p.external);
  p = parse(j::Object{{"mode", "normal"}, {"externalAiConsent", false}});
  EXPECT_TRUE(p.ai);
  EXPECT_FALSE(p.external);
  p = parse(j::Object{{"mode", "normal"}, {"externalAiConsent", true}});
  EXPECT_TRUE(p.ai);
  EXPECT_TRUE(p.external);
  p = parse(j::Object{{"externalAiConsent", true}});  // A missing mode is normal.
  EXPECT_TRUE(p.ai);
  EXPECT_TRUE(p.external);
  for (const auto* mode : {"private", "offline"}) {
    p = parse(j::Object{{"mode", mode}, {"externalAiConsent", true}});
    EXPECT_TRUE(p.ai) << mode;
    EXPECT_FALSE(p.external) << mode;
  }
  const auto custom = [&](bool consent, bool external_ai) {
    return parse(j::Object{{"mode", "custom"},
                           {"externalAiConsent", consent},
                           {"custom", j::Object{{"externalAi", external_ai}}}});
  };
  EXPECT_FALSE(custom(false, true).external);
  EXPECT_FALSE(custom(true, false).external);
  EXPECT_TRUE(custom(true, true).ai);
  EXPECT_TRUE(custom(true, true).external);
  // A consent value that is not a boolean turns AI off altogether.
  for (const auto& value : {j::Value("true"), j::Value(1), j::Value()}) {
    p = parse(j::Object{{"mode", "normal"}, {"externalAiConsent", value}});
    EXPECT_FALSE(p.ai);
    EXPECT_FALSE(p.external);
  }
}
}  // namespace
