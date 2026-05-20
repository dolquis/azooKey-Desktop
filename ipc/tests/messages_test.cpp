#include <string>

#include <gtest/gtest.h>

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
