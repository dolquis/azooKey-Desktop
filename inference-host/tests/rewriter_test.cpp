#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/Dispatcher.h"
#include "azookey/host/RewriterData.h"
#include "azookey/ipc/Payloads.h"

namespace azookey::host {
namespace {
class RewriterHostTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir = std::filesystem::temp_directory_path() /
          ("azookey-rewriter-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    config.rewriters.symbol_path = dir / "symbol.tsv";
    config.rewriters.emoji_path = dir / "emoji.tsv";
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  void WriteData() {
    std::ofstream(config.rewriters.symbol_path, std::ios::binary)
        << "「」\tかぎかっこ\tかぎ括弧\t1\n";
    std::ofstream(config.rewriters.emoji_path, std::ios::binary)
        << "😄\tわらい|かぎかっこ\tsmile\t笑顔\t1\n";
  }
  ipc::QueryCandidatesResponse Query(Dispatcher& dispatcher, std::string reading,
                                     std::string trigger = {}, uint32_t limit = 10,
                                     bool live = false) {
    ipc::QueryCandidatesRequest request;
    request.reading = reading;
    request.emoji_trigger = trigger;
    request.max_candidates = limit;
    request.live = live;
    ipc::Envelope envelope;
    envelope.request_id = ++request_id;
    envelope.type = ipc::MessageType::QueryCandidates;
    envelope.payload_json = ipc::BuildQueryCandidatesRequest(request);
    const auto response = dispatcher.Dispatch(envelope);
    EXPECT_TRUE(response);
    if (!response) return {};
    auto parsed = ipc::ParseQueryCandidatesResponse(response->payload_json);
    EXPECT_TRUE(parsed);
    return parsed.value_or(ipc::QueryCandidatesResponse{});
  }
  std::filesystem::path dir;
  EngineConfig config;
  uint64_t request_id{};
};

TEST_F(RewriterHostTest, DisabledNeverLoadsAndEnablingLazilyLoadsOnce) {
  RewriterData data;
  EXPECT_FALSE(data.Get(true, config.rewriters, nullptr));
  WriteData();
  config.rewriters.emoji_enabled = true;
  const auto index = data.Get(true, config.rewriters, nullptr);
  ASSERT_TRUE(index);
  EXPECT_EQ(index->SearchTrigger("smile", 12)[0].surface, "😄");
  std::filesystem::remove(config.rewriters.emoji_path);
  EXPECT_EQ(data.Get(true, config.rewriters, nullptr), index);
}

TEST_F(RewriterHostTest, MissingDataDoesNotRetryAndMalformedRowsAreSkipped) {
  RewriterData data;
  config.rewriters.emoji_enabled = true;
  config.rewriters.symbol_enabled = true;
  EXPECT_FALSE(data.Get(true, config.rewriters, nullptr));
  WriteData();
  EXPECT_FALSE(data.Get(true, config.rewriters, nullptr));
  std::ofstream(config.rewriters.symbol_path, std::ios::binary | std::ios::app) << "broken row\n";
  const auto symbols = data.Get(false, config.rewriters, nullptr);
  ASSERT_TRUE(symbols);
  ASSERT_EQ(symbols->size(), 1u);
  EXPECT_EQ(symbols->LookupReading("かぎかっこ")[0].surface, "「」");
}

TEST_F(RewriterHostTest, DispatcherMergesOnlyManualQueriesAndPreservesOrdinaryCount) {
  WriteData();
  InferenceEngine engine(std::make_unique<core::SimpleConverter>(), nullptr, config);
  RequestScheduler scheduler;
  Dispatcher dispatcher(&engine, &scheduler, nullptr);
  const auto off = Query(dispatcher, "かぎかっこ", {}, 2);
  config.rewriters.symbol_enabled = config.rewriters.emoji_enabled = true;
  engine.ApplyConfig(config);
  const auto on = Query(dispatcher, "かぎかっこ", {}, 2);
  ASSERT_EQ(on.candidates.size(), off.candidates.size() + 2);
  for (size_t i = 0; i < off.candidates.size(); ++i)
    EXPECT_EQ(on.candidates[i].surface, off.candidates[i].surface);
  EXPECT_EQ(on.candidates[on.candidates.size() - 2].source, "symbol");
  EXPECT_EQ(on.candidates.back().source, "emoji");
  EXPECT_EQ(on.candidates.back().description, "笑顔");
  const auto no_match_on = Query(dispatcher, "かな", {}, 9);
  config.rewriters.symbol_enabled = config.rewriters.emoji_enabled = false;
  engine.ApplyConfig(config);
  const auto no_match_off = Query(dispatcher, "かな", {}, 9);
  ASSERT_EQ(no_match_on.candidates.size(), no_match_off.candidates.size());
  EXPECT_EQ(Query(dispatcher, "かぎかっこ", {}, 2).candidates.size(), off.candidates.size());
  const auto live = Query(dispatcher, "かぎかっこ", {}, 2, true);
  for (const auto& candidate : live.candidates) {
    EXPECT_NE(candidate.source, "symbol");
    EXPECT_NE(candidate.source, "emoji");
  }
}

TEST_F(RewriterHostTest, PathChangeRecoversFailureAndPreservesReadersOfPreviousIndex) {
  RewriterData data;
  config.rewriters.emoji_enabled = true;
  EXPECT_FALSE(data.Get(true, config.rewriters, nullptr));
  config.rewriters.emoji_path = dir / "fixed.tsv";
  WriteData();
  const auto first = data.Get(true, config.rewriters, nullptr);
  ASSERT_TRUE(first);
  config.rewriters.emoji_path = dir / "replacement.tsv";
  std::ofstream(config.rewriters.emoji_path) << "X\t\tx\treplacement\t1\n";
  const auto second = data.Get(true, config.rewriters, nullptr);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->SearchTrigger("x", 1)[0].surface, "X");
  EXPECT_EQ(first->SearchTrigger("smile", 1)[0].surface, "😄");
  config.rewriters.emoji_enabled = false;
  EXPECT_FALSE(data.Get(true, config.rewriters, nullptr));
  config.rewriters.emoji_enabled = true;
  EXPECT_EQ(data.Get(true, config.rewriters, nullptr), second);
}

TEST_F(RewriterHostTest, NineOrdinaryCandidatesSurviveBothConfigurationDirections) {
  WriteData();
  const auto dictionary = dir / "ordinary.tsv";
  {
    std::ofstream out(dictionary);
    for (int i = 0; i < 20; ++i) out << "かな\tword" << i << "\t" << 30 - i << "\n";
  }
  auto converter = std::make_unique<core::SimpleConverter>();
  ASSERT_TRUE(converter->LoadFromTsv(dictionary.string()));
  InferenceEngine engine(std::move(converter), nullptr, config);
  RequestScheduler scheduler;
  Dispatcher dispatcher(&engine, &scheduler, nullptr);
  for (const bool enabled : {false, true, false, true}) {
    config.rewriters.symbol_enabled = config.rewriters.emoji_enabled = enabled;
    engine.ApplyConfig(config);
    const auto result = Query(dispatcher, "かな", {}, 9);
    ASSERT_EQ(result.candidates.size(), 9u);
    for (const auto& candidate : result.candidates) {
      EXPECT_NE(candidate.source, "symbol");
      EXPECT_NE(candidate.source, "emoji");
    }
  }
}

TEST_F(RewriterHostTest, TriggerHasNoReadingAndRejectsMixedRequests) {
  WriteData();
  config.rewriters.emoji_enabled = true;
  InferenceEngine engine(std::make_unique<core::SimpleConverter>(), nullptr, config);
  RequestScheduler scheduler;
  Dispatcher dispatcher(&engine, &scheduler, nullptr);
  const auto result = Query(dispatcher, {}, "smile", 1);
  ASSERT_EQ(result.candidates.size(), 1u);
  EXPECT_EQ(result.candidates[0].surface, "😄");
  EXPECT_TRUE(result.candidates[0].reading.empty());
  EXPECT_FALSE(result.partial);
  const auto invalid = Query(dispatcher, "わらい", "smile");
  EXPECT_TRUE(invalid.candidates.empty());
  EXPECT_FALSE(invalid.ok);
  ASSERT_TRUE(invalid.error);
  config.rewriters.emoji_enabled = false;
  engine.ApplyConfig(config);
  EXPECT_TRUE(Query(dispatcher, {}, "smile").candidates.empty());
}

TEST_F(RewriterHostTest, BundledDataRoundTripsAndFitsIndexBudget) {
  config.rewriters.symbol_enabled = config.rewriters.emoji_enabled = true;
  config.rewriters.symbol_path = std::filesystem::path(AZOOKEY_REWRITER_DATA_DIR) / "symbol.tsv";
  config.rewriters.emoji_path = std::filesystem::path(AZOOKEY_REWRITER_DATA_DIR) / "emoji.tsv";
  RewriterData data;
  const auto symbols = data.Get(false, config.rewriters, nullptr);
  const auto emoji = data.Get(true, config.rewriters, nullptr);
  ASSERT_TRUE(symbols);
  ASSERT_TRUE(emoji);
  EXPECT_GT(symbols->size(), 1000u);
  EXPECT_GT(emoji->size(), 1000u);
  for (const auto& path : {config.rewriters.symbol_path, config.rewriters.emoji_path}) {
    std::ifstream stream(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
    core::RewriterIndex index(path == config.rewriters.symbol_path ? core::CandidateSource::Symbol
                                                                   : core::CandidateSource::Emoji);
    EXPECT_EQ(index.Parse(bytes), 0u);
  }
  ASSERT_FALSE(symbols->LookupReading("かぎかっこ").empty());
  EXPECT_EQ(symbols->LookupReading("かぎかっこ")[0].surface, "「」");
  ASSERT_FALSE(emoji->LookupReading("わらい").empty());
  EXPECT_EQ(emoji->LookupReading("わらい")[0].surface, "😄");
  EXPECT_EQ(emoji->SearchTrigger("smile", 1)[0].surface, "😄");
  EXPECT_LE(emoji->EstimatedMemoryBytes(), 8u * 1024u * 1024u);
}
}  // namespace
}  // namespace azookey::host
