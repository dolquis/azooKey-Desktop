#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "azookey/ipc/Json.h"
#include "azookey/ipc/Limits.h"
#include "azookey/ipc/Messages.h"

TEST(MessagesTest, EnvelopeRoundTrip) {
  azookey::ipc::Envelope env;
  env.request_id = 42;
  env.trace_id = "t1";
  env.type = azookey::ipc::MessageType::QueryCandidates;
  env.payload_json = "{\"reading\":\"にほん\",\"max_candidates\":7}";

  const auto json = azookey::ipc::Serialize(env);
  auto decoded = azookey::ipc::Deserialize(json);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->request_id, 42u);
  EXPECT_EQ(decoded->trace_id, "t1");
  EXPECT_EQ(decoded->type, azookey::ipc::MessageType::QueryCandidates);
  EXPECT_NE(decoded->payload_json.find("にほん"), std::string::npos);
  EXPECT_NE(decoded->payload_json.find("max_candidates"), std::string::npos);

  // Re-serializing the decoded envelope must remain decodable.
  const auto rejson = azookey::ipc::Serialize(*decoded);
  auto redecoded = azookey::ipc::Deserialize(rejson);
  ASSERT_TRUE(redecoded.has_value());
  EXPECT_EQ(redecoded->payload_json, decoded->payload_json);
}

TEST(MessagesTest, TypeStringMapping) {
  EXPECT_EQ(azookey::ipc::TypeFromString("QueryPredictions"),
            azookey::ipc::MessageType::QueryPredictions);
  EXPECT_EQ(azookey::ipc::TypeFromString("QueryCorrections"),
            azookey::ipc::MessageType::QueryCorrections);
  EXPECT_EQ(azookey::ipc::TypeFromString("CommitCorrection"),
            azookey::ipc::MessageType::CommitCorrection);
  EXPECT_EQ(azookey::ipc::TypeFromString("UpdateUserWord"),
            azookey::ipc::MessageType::UpdateUserWord);
  EXPECT_EQ(azookey::ipc::TypeToString(azookey::ipc::MessageType::QueryPredictions),
            "QueryPredictions");
}

TEST(MessagesTest, LengthPrefixedFramingRoundTrip) {
  azookey::ipc::Envelope env;
  env.request_id = 42;
  env.type = azookey::ipc::MessageType::QueryCandidates;
  env.payload_json = "{}";
  const auto json = azookey::ipc::Serialize(env);

  auto lp = azookey::ipc::EncodeLengthPrefixed(json);
  auto restored = azookey::ipc::DecodeLengthPrefixed(lp);
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(*restored, json);
}

TEST(MessagesTest, MalformedInputRejected) {
  EXPECT_FALSE(azookey::ipc::Deserialize("not json").has_value());
}

TEST(JsonTest, RejectsMalformedBoundaryInputs) {
  namespace json = azookey::ipc::json;

  std::string too_deep = "0";
  for (size_t i = 0; i <= azookey::ipc::kMaxJsonNestDepth; ++i) {
    too_deep = "[" + too_deep + "]";
  }

  EXPECT_FALSE(json::Parse(std::string(azookey::ipc::kMaxJsonInputBytes + 1, ' ')));
  EXPECT_FALSE(json::Parse(too_deep));
  EXPECT_FALSE(json::Parse("{\"value\":1} trailing"));
  EXPECT_FALSE(json::Parse("{\"value\":1e9999}"));
  EXPECT_FALSE(json::Parse("{\"value\":9007199254740992}"));
  EXPECT_FALSE(json::Parse("{\"value\":01}"));
  EXPECT_FALSE(json::Parse("{\"value\":1e}"));
  EXPECT_FALSE(json::Parse(std::string("{\"value\":\"bad") + '\x01' + "\"}"));
  EXPECT_FALSE(json::Parse("{\"value\":\"\\uD800\"}"));
  EXPECT_FALSE(json::Parse("{\"value\":\"\\uDC00\"}"));
}

TEST(JsonTest, CombinesSurrogatePairsAndRejectsInvalidUtf8) {
  namespace json = azookey::ipc::json;

  auto parsed = json::Parse("{\"value\":\"\\uD83D\\uDE00\"}");
  ASSERT_TRUE(parsed.has_value());
  auto value = parsed->GetString("value");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, "\xF0\x9F\x98\x80");

  EXPECT_FALSE(json::Parse(std::string("{\"value\":\"") + '\xC0' + '\xAF' + "\"}"));
  EXPECT_FALSE(json::Parse(std::string("{\"value\":\"") + '\xE0' + '\x80' + '\x80' + "\"}"));
}

TEST(JsonTest, RandomBytesFailSafely) {
  namespace json = azookey::ipc::json;

  uint32_t state = 0xA5001234u;
  for (int i = 0; i < 256; ++i) {
    std::string input;
    const size_t length = static_cast<size_t>((state >> 24) & 0x3F);
    input.reserve(length);
    for (size_t j = 0; j < length; ++j) {
      state = state * 1664525u + 1013904223u;
      input.push_back(static_cast<char>((state >> 16) & 0xFF));
    }
    (void)json::Parse(input);
  }
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
  EXPECT_TRUE(azookey::ipc::EncodeLengthPrefixed(std::string(oversized, 'x')).empty());
}
