#include <string>

#include <gtest/gtest.h>

#include "azookey/ipc/Json.h"
#include "azookey/ipc/Payloads.h"

TEST(PayloadsTest, JsonEscapeAndRoundTrip) {
  // U+0001 is a control character with no named escape, so it must be emitted
  // as a \uXXXX sequence.
  const std::string src = std::string("「\"日本\\n語\"」\t") + '\x01';
  const auto escaped = azookey::ipc::json::EscapeString(src);
  EXPECT_NE(escaped.find("\\\""), std::string::npos);
  EXPECT_NE(escaped.find("\\n"), std::string::npos);
  EXPECT_NE(escaped.find("\\u0001"), std::string::npos);

  const std::string wrapped = std::string("\"") + escaped + "\"";
  auto v = azookey::ipc::json::Parse(wrapped);
  ASSERT_TRUE(v.has_value());
  ASSERT_TRUE(v->IsString());
  EXPECT_EQ(v->AsString(), src);
}

TEST(PayloadsTest, Handshake) {
  azookey::ipc::HandshakeRequest req;
  req.tip_version = "0.1.0";
  req.protocol_version = 1;
  req.capabilities = {"live_conversion", "cancel"};
  auto json = azookey::ipc::BuildHandshakeRequest(req);
  auto parsed = azookey::ipc::ParseHandshakeRequest(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->tip_version, "0.1.0");
  ASSERT_EQ(parsed->capabilities.size(), 2u);
  EXPECT_EQ(parsed->capabilities[0], "live_conversion");

  azookey::ipc::HandshakeResponse res;
  res.host_version = "0.1.0";
  res.accepted = true;
  res.model_loaded = false;
  auto json2 = azookey::ipc::BuildHandshakeResponse(res);
  auto parsed2 = azookey::ipc::ParseHandshakeResponse(json2);
  ASSERT_TRUE(parsed2.has_value());
  EXPECT_TRUE(parsed2->accepted);
  EXPECT_FALSE(parsed2->model_loaded);
}

TEST(PayloadsTest, Ping) {
  azookey::ipc::PingPayload p;
  p.nonce = 12345;
  p.t_ms = 1700000000123ULL;
  auto json = azookey::ipc::BuildPing(p);
  auto parsed = azookey::ipc::ParsePing(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->nonce, 12345u);
  EXPECT_EQ(parsed->t_ms, 1700000000123ULL);
}

TEST(PayloadsTest, Health) {
  azookey::ipc::HealthPayload p;
  p.status = "degraded";
  p.backend = "cpu";
  p.model_loaded = false;
  p.vram_mb = 0;
  p.last_error = "no cuda runtime";
  auto json = azookey::ipc::BuildHealth(p);
  auto parsed = azookey::ipc::ParseHealth(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->status, "degraded");
  EXPECT_EQ(parsed->backend, "cpu");
  ASSERT_TRUE(parsed->last_error.has_value());
  EXPECT_EQ(*parsed->last_error, "no cuda runtime");
}

TEST(PayloadsTest, LoadModel) {
  azookey::ipc::LoadModelRequest req;
  req.path = "C:\\models\\zenz.gguf";
  req.backend = "cuda";
  req.n_gpu_layers = 32;
  auto json = azookey::ipc::BuildLoadModelRequest(req);
  auto parsed = azookey::ipc::ParseLoadModelRequest(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->path, "C:\\models\\zenz.gguf");
  ASSERT_TRUE(parsed->n_gpu_layers.has_value());
  EXPECT_EQ(*parsed->n_gpu_layers, 32);

  azookey::ipc::LoadModelResponse res;
  res.ok = false;
  res.error = "file not found";
  auto json2 = azookey::ipc::BuildLoadModelResponse(res);
  auto parsed2 = azookey::ipc::ParseLoadModelResponse(json2);
  ASSERT_TRUE(parsed2.has_value());
  EXPECT_FALSE(parsed2->ok);
  EXPECT_TRUE(parsed2->error.has_value());
}

TEST(PayloadsTest, QueryCandidates) {
  azookey::ipc::QueryCandidatesRequest req;
  req.reading = "にほんご";
  req.left_context = "私は";
  req.max_candidates = 5;
  req.live = true;
  auto json = azookey::ipc::BuildQueryCandidatesRequest(req);
  auto parsed = azookey::ipc::ParseQueryCandidatesRequest(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->reading, "にほんご");
  EXPECT_EQ(parsed->max_candidates, 5);
  EXPECT_TRUE(parsed->live);

  azookey::ipc::QueryCandidatesResponse res;
  res.candidates = {
      {"日本語", "にほんご", 1.0, "static-dict"},
      {"日本五", "にほんご", 0.3, "fallback"},
  };
  res.partial = false;
  auto json2 = azookey::ipc::BuildQueryCandidatesResponse(res);
  auto parsed2 = azookey::ipc::ParseQueryCandidatesResponse(json2);
  ASSERT_TRUE(parsed2.has_value());
  ASSERT_EQ(parsed2->candidates.size(), 2u);
  EXPECT_EQ(parsed2->candidates[0].surface, "日本語");
  EXPECT_EQ(parsed2->candidates[0].score, 1.0);
}

TEST(PayloadsTest, Cancel) {
  azookey::ipc::CancelPayload p;
  p.target_request_id = 7777;
  auto json = azookey::ipc::BuildCancel(p);
  auto parsed = azookey::ipc::ParseCancel(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->target_request_id, 7777u);
}

TEST(PayloadsTest, CommitObservation) {
  azookey::ipc::CommitObservationRequest req;
  req.reading = "にほんご";
  req.chosen = {"日本語", "にほんご", 1.0, "user"};
  req.shown = {
      {"日本語", "にほんご", 1.0, "static-dict"},
      {"二本後", "にほんご", 0.1, "fallback"},
  };
  req.left_context = "";
  req.timestamp_ms = 1700000000123ULL;
  auto json = azookey::ipc::BuildCommitObservationRequest(req);
  auto parsed = azookey::ipc::ParseCommitObservationRequest(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->chosen.surface, "日本語");
  EXPECT_EQ(parsed->shown.size(), 2u);
  EXPECT_EQ(parsed->timestamp_ms, 1700000000123ULL);
}

TEST(PayloadsTest, UserWord) {
  azookey::ipc::AddUserWordRequest add;
  add.word = "azooKey";
  add.ruby = "あずきい";
  add.cid = 1285;
  add.value = -5.0;
  auto json = azookey::ipc::BuildAddUserWordRequest(add);
  auto parsed = azookey::ipc::ParseAddUserWordRequest(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->word, "azooKey");
  ASSERT_TRUE(parsed->cid.has_value());
  EXPECT_EQ(*parsed->cid, 1285);
  EXPECT_TRUE(parsed->value.has_value());

  azookey::ipc::RemoveUserWordRequest rm;
  rm.word = "azooKey";
  rm.ruby = "あずきい";
  auto json2 = azookey::ipc::BuildRemoveUserWordRequest(rm);
  auto parsed2 = azookey::ipc::ParseRemoveUserWordRequest(json2);
  ASSERT_TRUE(parsed2.has_value());
  EXPECT_EQ(parsed2->word, "azooKey");
}

TEST(PayloadsTest, MalformedRejection) {
  EXPECT_FALSE(azookey::ipc::ParseHandshakeRequest("not json").has_value());
  EXPECT_FALSE(azookey::ipc::ParseQueryCandidatesRequest("{}").has_value());
  EXPECT_FALSE(azookey::ipc::ParseCancel("{}").has_value());
}
