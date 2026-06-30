#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

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

TEST(MessagesTest, EnvelopeRoundTripPreservesFullUInt64RequestId) {
  azookey::ipc::Envelope env;
  env.request_id = std::numeric_limits<uint64_t>::max();
  env.trace_id = "large";
  env.type = azookey::ipc::MessageType::Ping;
  env.payload_json = "{}";

  const auto serialized = azookey::ipc::Serialize(env);
  EXPECT_NE(serialized.find("\"request_id\":18446744073709551615"), std::string::npos);

  auto decoded = azookey::ipc::Deserialize(serialized);
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

  auto lp = azookey::ipc::EncodeLengthPrefixed(json);
  ASSERT_TRUE(lp.has_value());
  auto restored = azookey::ipc::DecodeLengthPrefixed(*lp);
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(*restored, json);
}

TEST(MessagesTest, MalformedInputRejected) {
  EXPECT_FALSE(azookey::ipc::Deserialize("not json").has_value());
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
