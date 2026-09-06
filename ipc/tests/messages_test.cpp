#include "azookey/ipc/Messages.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

#include "azookey/ipc/Json.h"
#include "azookey/ipc/Limits.h"

TEST(MessagesTest, EnvelopeRoundTrip) {
  azookey::ipc::Envelope env;
  env.request_id = 42;
  env.trace_id = "t1";
  env.type = azookey::ipc::MessageType::QueryCandidates;
  env.payload_json = "{\"reading\":\"にほん\",\"max_candidates\":7}";

  const auto json = azookey::ipc::Serialize(env);
  ASSERT_TRUE(json.has_value());
  auto decoded = azookey::ipc::Deserialize(*json);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->request_id, 42u);
  EXPECT_EQ(decoded->trace_id, "t1");
  EXPECT_EQ(decoded->type, azookey::ipc::MessageType::QueryCandidates);
  EXPECT_NE(decoded->payload_json.find("にほん"), std::string::npos);
  EXPECT_NE(decoded->payload_json.find("max_candidates"), std::string::npos);

  // Re-serializing the decoded envelope must remain decodable.
  const auto rejson = azookey::ipc::Serialize(*decoded);
  ASSERT_TRUE(rejson.has_value());
  auto redecoded = azookey::ipc::Deserialize(*rejson);
  ASSERT_TRUE(redecoded.has_value());
  EXPECT_EQ(redecoded->payload_json, decoded->payload_json);
}

TEST(MessagesTest, EnvelopeRoundTripPreservesFullUInt64RequestId) {
  azookey::ipc::Envelope env;
  env.request_id = std::numeric_limits<uint64_t>::max();
  env.trace_id = "large";
  env.type = azookey::ipc::MessageType::Ping;
  env.payload_json = "{}";

  const auto serialized = azookey::ipc::Serialize(env);
  ASSERT_TRUE(serialized.has_value());
  EXPECT_NE(serialized->find("\"request_id\":18446744073709551615"), std::string::npos);

  auto decoded = azookey::ipc::Deserialize(*serialized);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->request_id, std::numeric_limits<uint64_t>::max());
}

TEST(MessagesTest, TypeStringMapping) {
  EXPECT_EQ(azookey::ipc::TypeFromString("QueryPredictions"),
            azookey::ipc::MessageType::QueryPredictions);
  EXPECT_EQ(azookey::ipc::TypeFromString("QueryBatchConversion"),
            azookey::ipc::MessageType::QueryBatchConversion);
  EXPECT_EQ(azookey::ipc::TypeFromString("QueryCorrections"),
            azookey::ipc::MessageType::QueryCorrections);
  EXPECT_EQ(azookey::ipc::TypeFromString("CommitCorrection"),
            azookey::ipc::MessageType::CommitCorrection);
  EXPECT_EQ(azookey::ipc::TypeFromString("UpdateUserWord"),
            azookey::ipc::MessageType::UpdateUserWord);
  EXPECT_EQ(azookey::ipc::TypeFromString("UpdateConfig"),
            azookey::ipc::MessageType::UpdateConfig);
  EXPECT_EQ(azookey::ipc::TypeToString(azookey::ipc::MessageType::QueryPredictions),
            "QueryPredictions");
  EXPECT_EQ(azookey::ipc::TypeToString(azookey::ipc::MessageType::QueryBatchConversion),
            "QueryBatchConversion");
  EXPECT_EQ(azookey::ipc::TypeToString(azookey::ipc::MessageType::UpdateConfig),
            "UpdateConfig");
}

TEST(MessagesTest, LengthPrefixedFramingRoundTrip) {
  azookey::ipc::Envelope env;
  env.request_id = 42;
  env.type = azookey::ipc::MessageType::QueryCandidates;
  env.payload_json = "{}";
  const auto json = azookey::ipc::Serialize(env);
  ASSERT_TRUE(json.has_value());

  auto lp = azookey::ipc::EncodeLengthPrefixed(*json);
  ASSERT_TRUE(lp.has_value());
  auto restored = azookey::ipc::DecodeLengthPrefixed(*lp);
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(*restored, *json);
}

TEST(MessagesTest, MalformedInputRejected) {
  EXPECT_FALSE(azookey::ipc::Deserialize("not json").has_value());
}

TEST(MessagesTest, PayloadSpellingAndEscapedEnvelopeFieldsSurviveRoundTrip) {
  for (const std::string payload :
       {R"({ "z": [1e2,18446744073709551615], "a":"\u3042" })",
        R"([true,null,{"payload":"nested"}])", R"("a\"b")", "null", "false", "1.25e2"}) {
    azookey::ipc::Envelope env;
    env.request_id = 5;
    env.trace_id = "quote\"slash\\\n";
    env.payload_json = payload;
    auto encoded = azookey::ipc::Serialize(env);
    ASSERT_TRUE(encoded.has_value());
    auto decoded = azookey::ipc::Deserialize(*encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->payload_json, payload);
    EXPECT_EQ(decoded->trace_id, env.trace_id);
  }
}

TEST(MessagesTest, RawPayloadUsesFirstDecodedTopLevelKeyAndOwnsItsBytes) {
  auto decoded = azookey::ipc::Deserialize(
      R"({"nested":{"payload":9},"paylo\u0061d": { "x": 1 },"payload":2,"request_id":1,"trace_id":"","type":"Ping"})");
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->payload_json, R"({ "x": 1 })");
  EXPECT_FALSE(azookey::ipc::Deserialize(
                   R"({"payload":{},"request_id":1,"trace_id":"","type":"Ping"} trailing)")
                   .has_value());
}

TEST(MessagesTest, SplicedPayloadCannotInjectEnvelopeFields) {
  azookey::ipc::Envelope env;
  for (const std::string payload : {R"({},"request_id":99)", "{} {}", "[1,]", "1e9999"}) {
    env.payload_json = payload;
    EXPECT_FALSE(azookey::ipc::Serialize(env).has_value());
  }
  std::string_view raw = "old";
  EXPECT_FALSE(azookey::ipc::json::ParseWithRawMember(R"({"payload":{} garbage})", "payload", raw)
                   .has_value());
  EXPECT_TRUE(raw.empty());
}

TEST(MessagesTest, SerializeRejectsMalformedPayload) {
  azookey::ipc::Envelope env;
  env.request_id = 1;
  env.type = azookey::ipc::MessageType::QueryCandidates;
  env.payload_json = "{not valid json";
  EXPECT_FALSE(azookey::ipc::Serialize(env).has_value());
}

TEST(MessagesTest, SerializeAcceptsEmptyPayload) {
  azookey::ipc::Envelope env;
  env.request_id = 1;
  env.type = azookey::ipc::MessageType::Ping;
  // Empty payload_json is valid and encodes as an empty object.
  const auto json = azookey::ipc::Serialize(env);
  ASSERT_TRUE(json.has_value());
  EXPECT_NE(json->find("\"payload\":{}"), std::string::npos);
}

TEST(MessagesTest, DeserializeRejectsUnsupportedVersion) {
  // A future breaking generation must be dropped, not misinterpreted.
  const std::string future =
      "{\"version\":2,\"request_id\":1,\"trace_id\":\"t\",\"type\":\"Ping\",\"payload\":{}}";
  EXPECT_FALSE(azookey::ipc::Deserialize(future).has_value());

  // A non-positive version is malformed.
  const std::string zero =
      "{\"version\":0,\"request_id\":1,\"trace_id\":\"t\",\"type\":\"Ping\",\"payload\":{}}";
  EXPECT_FALSE(azookey::ipc::Deserialize(zero).has_value());
}

TEST(MessagesTest, DeserializeAcceptsSupportedVersion) {
  const std::string current =
      "{\"version\":1,\"request_id\":1,\"trace_id\":\"t\",\"type\":\"Ping\",\"payload\":{}}";
  auto decoded = azookey::ipc::Deserialize(current);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->version, azookey::ipc::kEnvelopeVersion);
}

TEST(MessagesTest, LengthPrefixedFramingRejectsOversizedFrames) {
  std::vector<uint8_t> bytes(4, 0);
  const uint32_t oversized = azookey::ipc::kMaxFrameSize + 1;
  bytes[0] = static_cast<uint8_t>(oversized & 0xFF);
  bytes[1] = static_cast<uint8_t>((oversized >> 8) & 0xFF);
  bytes[2] = static_cast<uint8_t>((oversized >> 16) & 0xFF);
  bytes[3] = static_cast<uint8_t>((oversized >> 24) & 0xFF);
  bytes.resize(static_cast<size_t>(oversized) + 4, 'x');

  EXPECT_FALSE(azookey::ipc::DecodeLengthPrefixed(bytes).has_value());
  EXPECT_FALSE(azookey::ipc::EncodeLengthPrefixed(std::string(oversized, 'x')).has_value());
}
