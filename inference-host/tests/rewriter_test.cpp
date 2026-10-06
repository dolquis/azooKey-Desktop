#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
  // Twenty ordinary candidates for かな, so a limit below twenty is always honoured.
  std::unique_ptr<core::SimpleConverter> OrdinaryConverter() {
    const auto dictionary = dir / "ordinary.tsv";
    {
      std::ofstream out(dictionary);
      for (int i = 0; i < 20; ++i) out << "かな\tword" << i << "\t" << 30 - i << "\n";
    }
    auto converter = std::make_unique<core::SimpleConverter>();
    EXPECT_TRUE(converter->LoadFromTsv(dictionary.string()));
    return converter;
  }
  // Six symbols and six emoji read かな; only the four highest ranked of each may be merged.
  void WriteTailData() {
    std::ofstream(config.rewriters.symbol_path, std::ios::binary)
        << "★\tかな\t黒星\t60\n☆\tかな\t白星\t50\n○\tかな\t白丸\t40\n"
           "●\tかな\t黒丸\t30\n◎\tかな\t二重丸\t20\n◇\tかな\t白菱形\t10\n";
    std::ofstream(config.rewriters.emoji_path, std::ios::binary)
        << "😀\tかな\tgrin\t笑顔\t60\n😃\tかな\tgrinning\t大きな笑顔\t50\n"
           "😄\tかな\tsmile\t目が笑う笑顔\t40\n😁\tかな\tbeam\t歯を見せた笑顔\t30\n"
           "😆\tかな\tlaugh\t大笑い\t20\n😅\tかな\tsweat\t冷や汗\t10\n";
  }
  static std::vector<std::string> Surfaces(const ipc::QueryCandidatesResponse& response) {
    std::vector<std::string> result;
    for (const auto& candidate : response.candidates) result.push_back(candidate.surface);
    return result;
  }
  static std::vector<std::string> Sources(const ipc::QueryCandidatesResponse& response) {
    std::vector<std::string> result;
    for (const auto& candidate : response.candidates) result.push_back(candidate.source);
    return result;
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

// Spec 9.5 / 19.5 / 19.12: every on/off combination keeps the ordinary candidates and appends
// at most four per enabled rewriter, symbols before emoji.
TEST_F(RewriterHostTest, EnablementMatrixKeepsOrdinaryPrefixAndAppendsFourPerEnabledRewriter) {
  WriteTailData();
  InferenceEngine engine(OrdinaryConverter(), nullptr, config);
  RequestScheduler scheduler;
  Dispatcher dispatcher(&engine, &scheduler, nullptr);
  const std::vector<std::string> symbols{"★", "☆", "○", "●"};
  const std::vector<std::string> emoji{"😀", "😃", "😄", "😁"};
  for (const uint32_t limit : {1u, 3u, 9u}) {
    config.rewriters.symbol_enabled = config.rewriters.emoji_enabled = false;
    engine.ApplyConfig(config);
    const auto off = Query(dispatcher, "かな", {}, limit);
    ASSERT_FALSE(off.candidates.empty());
    for (const bool symbol_on : {false, true, true, false}) {
      for (const bool emoji_on : {false, true}) {
        config.rewriters.symbol_enabled = symbol_on;
        config.rewriters.emoji_enabled = emoji_on;
        engine.ApplyConfig(config);
        const auto on = Query(dispatcher, "かな", {}, limit);
        auto expected_surfaces = Surfaces(off);
        auto expected_sources = Sources(off);
        if (symbol_on) {
          expected_surfaces.insert(expected_surfaces.end(), symbols.begin(), symbols.end());
          expected_sources.insert(expected_sources.end(), 4, "symbol");
        }
        if (emoji_on) {
          expected_surfaces.insert(expected_surfaces.end(), emoji.begin(), emoji.end());
          expected_sources.insert(expected_sources.end(), 4, "emoji");
        }
        EXPECT_EQ(Surfaces(on), expected_surfaces)
            << "limit=" << limit << " symbol=" << symbol_on << " emoji=" << emoji_on;
        EXPECT_EQ(Sources(on), expected_sources);
        EXPECT_TRUE(on.ok);
        EXPECT_FALSE(on.partial);
        for (const auto& candidate : on.candidates)
          if (candidate.source == "symbol" || candidate.source == "emoji")
            EXPECT_FALSE(candidate.description.empty());
      }
    }
  }
}

// Spec 14 / 19.10: a missing, unreadable or empty data file disables only that rewriter. The
// other rewriter and the ordinary candidates are unaffected and nothing throws.
TEST_F(RewriterHostTest, MissingOrDegenerateDataDisablesOnlyThatRewriter) {
  const std::vector<std::pair<std::string, std::string>> degenerate{
      {"empty file", ""},
      {"bom only", "\xef\xbb\xbf"},
      {"comments only", "# header\n# another\n"},
      {"all rows invalid", "broken row\nstill\tbroken\n"},
  };
  const auto expect_ordinary_only = [&](const char* label) {
    InferenceEngine engine(OrdinaryConverter(), nullptr, config);
    RequestScheduler scheduler;
    Dispatcher dispatcher(&engine, &scheduler, nullptr);
    config.rewriters.symbol_enabled = config.rewriters.emoji_enabled = false;
    engine.ApplyConfig(config);
    const auto off = Query(dispatcher, "かな", {}, 9);
    config.rewriters.symbol_enabled = config.rewriters.emoji_enabled = true;
    engine.ApplyConfig(config);
    for (int round = 0; round < 2; ++round) {
      const auto on = Query(dispatcher, "かな", {}, 9);
      EXPECT_EQ(Surfaces(on), Surfaces(off)) << label;
      EXPECT_EQ(Sources(on), Sources(off)) << label;
      EXPECT_TRUE(on.ok) << label;
    }
    EXPECT_TRUE(Query(dispatcher, {}, "smile", 5).candidates.empty()) << label;
  };

  expect_ordinary_only("both files missing");
  for (const auto& [label, bytes] : degenerate) {
    std::ofstream(config.rewriters.symbol_path, std::ios::binary) << bytes;
    std::ofstream(config.rewriters.emoji_path, std::ios::binary) << bytes;
    expect_ordinary_only(label.c_str());
  }
  std::filesystem::remove(config.rewriters.symbol_path);
  std::filesystem::remove(config.rewriters.emoji_path);
  std::filesystem::create_directory(config.rewriters.symbol_path);
  std::filesystem::create_directory(config.rewriters.emoji_path);
  expect_ordinary_only("paths are directories");
  std::filesystem::remove(config.rewriters.symbol_path);
  std::filesystem::remove(config.rewriters.emoji_path);

  // One file present, the other missing: the present one still contributes its four.
  for (const bool symbol_present : {true, false}) {
    WriteTailData();
    std::filesystem::remove(symbol_present ? config.rewriters.emoji_path
                                           : config.rewriters.symbol_path);
    InferenceEngine engine(OrdinaryConverter(), nullptr, config);
    RequestScheduler scheduler;
    Dispatcher dispatcher(&engine, &scheduler, nullptr);
    config.rewriters.symbol_enabled = config.rewriters.emoji_enabled = false;
    engine.ApplyConfig(config);
    const auto off = Query(dispatcher, "かな", {}, 9);
    config.rewriters.symbol_enabled = config.rewriters.emoji_enabled = true;
    engine.ApplyConfig(config);
    const auto on = Query(dispatcher, "かな", {}, 9);
    ASSERT_EQ(on.candidates.size(), off.candidates.size() + 4);
    for (size_t i = 0; i < off.candidates.size(); ++i)
      EXPECT_EQ(on.candidates[i].surface, off.candidates[i].surface);
    for (size_t i = off.candidates.size(); i < on.candidates.size(); ++i)
      EXPECT_EQ(on.candidates[i].source, symbol_present ? "symbol" : "emoji");
  }
}

// Spec 9.3 / 9.4 / 9.5: the ordering table holds end to end, and the request limit
// (emojiMaxCandidates) truncates only after sorting. Symbols never answer a :trigger search.
TEST_F(RewriterHostTest, TriggerSearchFollowsOrderingTableAndRequestLimit) {
  std::ofstream(config.rewriters.emoji_path, std::ios::binary)
      << "😄\t\tsmile|smiley\t笑顔\t10\n😆\t\tsmile\t大笑い\t10\n🙂\t\tsmile\t微笑み\t20\n"
         "😊\t\tsmiles\t笑み\t99\n😺\t\tsmilecat\t猫の笑顔\t500\n"
         "😁\t\tasmile\t歯を見せた笑顔\t1000\n🤣\t\tsmxile\t爆笑\t5\n🐍\t\tsnake\t蛇\t1\n";
  std::ofstream(config.rewriters.symbol_path, std::ios::binary)
      << "「」\tかぎかっこ\tかぎ括弧\t1\n";
  config.rewriters.symbol_enabled = config.rewriters.emoji_enabled = true;
  InferenceEngine engine(OrdinaryConverter(), nullptr, config);
  RequestScheduler scheduler;
  Dispatcher dispatcher(&engine, &scheduler, nullptr);
  const std::vector<std::string> all{"🙂", "😄", "😆", "😊", "😺", "😁", "🤣"};
  for (const uint32_t limit : {0u, 50u, 7u}) {
    const auto result = Query(dispatcher, {}, "smile", limit);
    EXPECT_EQ(Surfaces(result), all) << "limit=" << limit;
    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.partial);
    for (const auto& candidate : result.candidates) {
      EXPECT_EQ(candidate.source, "emoji");
      EXPECT_TRUE(candidate.reading.empty());
      EXPECT_FALSE(candidate.description.empty());
    }
  }
  EXPECT_EQ(Surfaces(Query(dispatcher, {}, "smile", 1)), (std::vector<std::string>{"🙂"}));
  EXPECT_EQ(Surfaces(Query(dispatcher, {}, "smile", 4)),
            (std::vector<std::string>{"🙂", "😄", "😆", "😊"}));
  EXPECT_EQ(Surfaces(Query(dispatcher, {}, "SMILE", 2)), (std::vector<std::string>{"🙂", "😄"}));
  EXPECT_TRUE(Query(dispatcher, {}, "smlie", 12).candidates.empty());
  EXPECT_EQ(Query(dispatcher, {}, "s", 12).candidates.size(), 7u);  // Exact/Prefix only
  EXPECT_TRUE(Query(dispatcher, {}, "かぎかっこ", 12).candidates.empty());

  // A live conversion request never searches triggers, whatever the settings are.
  EXPECT_TRUE(Query(dispatcher, {}, "smile", 12, true).candidates.empty());
  config.rewriters.trigger_enabled = false;
  engine.ApplyConfig(config);
  EXPECT_TRUE(Query(dispatcher, {}, "smile", 12).candidates.empty());
  config.rewriters.trigger_enabled = true;
  config.rewriters.emoji_enabled = false;
  engine.ApplyConfig(config);
  EXPECT_TRUE(Query(dispatcher, {}, "smile", 12).candidates.empty());
}

TEST_F(RewriterHostTest, TriggerSearchWithMissingEmojiDataIsEmptyNotAnError) {
  config.rewriters.symbol_enabled = config.rewriters.emoji_enabled = true;
  InferenceEngine engine(OrdinaryConverter(), nullptr, config);
  RequestScheduler scheduler;
  Dispatcher dispatcher(&engine, &scheduler, nullptr);
  const auto result = Query(dispatcher, {}, "smile", 12);
  EXPECT_TRUE(result.candidates.empty());
  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.error);
  EXPECT_FALSE(result.partial);
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
