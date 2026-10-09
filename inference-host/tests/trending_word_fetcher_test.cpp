#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azookey/core/Utf8.h"
#include "azookey/host/TrendingWordFetcher.h"
#include "azookey/ipc/Json.h"
#include "azookey/learning/AtomicFile.h"

namespace {
using namespace std::chrono_literals;
using azookey::host::HttpDownloadRequest;
using azookey::host::HttpDownloadResult;
using azookey::host::HttpDownloadStatus;
using azookey::host::HttpTextRequest;
using azookey::host::HttpTextResult;
using azookey::host::TrendingFetchDependencies;
using azookey::host::TrendingFetchStatus;
using azookey::host::TrendingWordFetcher;
using azookey::learning::AutoWordSource;
using azookey::learning::AutoWordState;
using azookey::learning::AutoWordStore;

const std::string kAsset = R"({"version":1,"generated_at":"2026-10-09T00:00:00Z","words":[
  {"surface":"推し活","reading":"おしかつ","rank":1},
  {"surface":"新語","reading":"しんご","rank":3},
  {"surface":"読みなし","rank":2}]})";

std::string AssetWithWords(std::string_view words) {
  return R"({"version":1,"generated_at":"2026-10-09T00:00:00Z","words":[)" + std::string(words) +
         "]}";
}

std::string AssetWord(std::string_view surface, std::string_view reading) {
  return "{\"surface\":\"" + azookey::ipc::json::EscapeString(surface) + "\",\"reading\":\"" +
         azookey::ipc::json::EscapeString(reading) + "\",\"rank\":1}";
}

std::string AssetWithWordCount(size_t count, bool skipped_readings) {
  const auto word = AssetWord("新語", "しんご");
  std::string words = word;
  for (size_t i = 1; i < count; ++i) {
    words += ',';
    words += skipped_readings ? (i % 2 == 0 ? R"({"surface":"読みなし"})"
                                            : R"({"surface":"空読み","reading":""})")
                              : word;
  }
  return AssetWithWords(words);
}

std::string Utf8Scalar(char32_t codepoint) {
  std::string text;
  azookey::core::AppendUtf8(text, codepoint);
  return text;
}

class TrendingWordFetcherTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<uint64_t> serial{0};
    directory_ = std::filesystem::temp_directory_path() /
                 ("azookey-trending-" + std::to_string(GetCurrentProcessId()) + "-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                  "-" + std::to_string(++serial));
    std::filesystem::create_directories(directory_);
    store_ = std::make_unique<AutoWordStore>(directory_ / "auto_words.tsv");
    ASSERT_TRUE(store_->Load());
    SetAsset(kAsset);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(directory_, ec);
  }
  void SetAsset(const std::string& text) {
    source_ = text;
    ASSERT_TRUE(azookey::learning::WriteTextFileAtomically(directory_ / "source.json", text));
    std::string error;
    const auto hash = azookey::host::ComputeFileSha256(directory_ / "source.json", &error);
    ASSERT_TRUE(hash.has_value()) << error;
    checksum_ = *hash + "  trending-words.json\n";
  }
  TrendingFetchDependencies Dependencies() {
    TrendingFetchDependencies dependencies;
    dependencies.fetch_text = [this](const HttpTextRequest& request) {
      ++text_calls_;
      EXPECT_EQ(request.url, L"https://fixture.invalid/trending-words.json.sha256");
      EXPECT_EQ(request.max_bytes, 1024u);
      EXPECT_TRUE(static_cast<bool>(request.cancelled));
      return HttpTextResult{checksum_, {}};
    };
    dependencies.download = [this](const HttpDownloadRequest& request) {
      ++download_calls_;
      EXPECT_EQ(request.url, L"https://fixture.invalid/trending-words.json");
      EXPECT_EQ(request.max_bytes, 1024u * 1024);
      EXPECT_TRUE(static_cast<bool>(request.cancelled));
      std::filesystem::copy_file(directory_ / "source.json", request.destination,
                                 std::filesystem::copy_options::overwrite_existing);
      return HttpDownloadResult{HttpDownloadStatus::Downloaded};
    };
    dependencies.now_epoch = [this] { return epoch_; };
    // Parsing/asset limits must not depend on instrumentation or machine speed.
    // Deadline and worker interval tests advance this clock explicitly.
    dependencies.steady_now = [] { return std::chrono::steady_clock::time_point{}; };
    return dependencies;
  }
  std::filesystem::path Cache() const { return directory_ / "trending-words.json"; }
  std::string ReadCache() const {
    std::ifstream input(Cache(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
  }
  void SeedExisting() {
    store_->Observe("既存語", "きぞんご", 10, 3, false);
    ASSERT_TRUE(store_->Save());
    ASSERT_TRUE(azookey::learning::WriteTextFileAtomically(Cache(), "old cache"));
  }
  void ExpectRejectedAssetPreservesStoreAndCache(const std::string& text) {
    const auto before = store_->SerializeText();
    const auto cache = ReadCache();
    SetAsset(text);
    TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                                Dependencies());
    fetcher.UpdateSettings(true, 24, true);
    EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Failed);
    EXPECT_EQ(store_->SerializeText(), before);
    EXPECT_EQ(ReadCache(), cache);
  }
  std::filesystem::path directory_;
  std::unique_ptr<AutoWordStore> store_;
  std::string source_;
  std::string checksum_;
  uint64_t epoch_{100};
  std::atomic<unsigned> text_calls_{0};
  std::atomic<unsigned> download_calls_{0};
};

TEST_F(TrendingWordFetcherTest, LocalFixtureIsVerifiedCachedAndPersisted) {
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              Dependencies());
  fetcher.UpdateSettings(true, 24, false);
  ASSERT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Ingested);
  EXPECT_EQ(ReadCache(), kAsset);
  const auto words = store_->ListByState(AutoWordState::Pending);
  ASSERT_EQ(words.size(), 2u);
  EXPECT_EQ(words[0].source, AutoWordSource::Trending);
  EXPECT_EQ(words[0].count, 3u);
  EXPECT_GT(words[0].score, words[1].score);
  AutoWordStore reloaded(directory_ / "auto_words.tsv");
  ASSERT_TRUE(reloaded.Load());
  EXPECT_EQ(reloaded.SerializeText(), store_->SerializeText());
  EXPECT_FALSE(std::filesystem::exists(Cache().wstring() + L".download"));
}

TEST_F(TrendingWordFetcherTest, AssetByteLimitMatchesJsonParserAndPreservesStoreOnOverflow) {
  std::string text = kAsset;
  text.resize(1024u * 1024, ' ');
  SetAsset(text);
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              Dependencies());
  fetcher.UpdateSettings(true, 24, false);
  ASSERT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Ingested);
  const auto before = store_->SerializeText();
  const auto cache = ReadCache();
  text += ' ';
  SetAsset(text);
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Failed);
  EXPECT_EQ(store_->SerializeText(), before);
  EXPECT_EQ(ReadCache(), cache);
}

TEST_F(TrendingWordFetcherTest, HostCommitCallbackOwnsIngestionAndFailedCommitCanRetry) {
  SeedExisting();
  const auto before = store_->SerializeText();
  auto dependencies = Dependencies();
  unsigned commits = 0;
  dependencies.ingest_and_save = [&](const std::vector<azookey::learning::AutoWord>& words,
                                     uint64_t now_epoch, bool auto_promote) {
    ++commits;
    EXPECT_EQ(words.size(), 2u);
    EXPECT_EQ(now_epoch, epoch_);
    EXPECT_TRUE(auto_promote);
    EXPECT_EQ(ReadCache(), kAsset);
    if (commits == 1) return false;
    store_->IngestTrending(words, now_epoch, auto_promote);
    return store_->Save();
  };
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              std::move(dependencies));
  fetcher.UpdateSettings(true, 24, true);
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Failed);
  EXPECT_EQ(store_->SerializeText(), before);
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Ingested);
  EXPECT_EQ(store_->LookupConfirmed("おしかつ").size(), 1u);
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Unchanged);
  EXPECT_EQ(commits, 2u);
}

TEST_F(TrendingWordFetcherTest, DisabledNeverEntersEitherSocketCreatingTransport) {
  auto dependencies = Dependencies();
  dependencies.fetch_text = [this](const HttpTextRequest&) {
    ++text_calls_;
    ADD_FAILURE() << "disabled fetcher reached WinHTTP session creation boundary";
    return HttpTextResult{{}, "unexpected network"};
  };
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              std::move(dependencies));
  fetcher.Start();
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Disabled);
  fetcher.UpdateSettings(false, 1, true);
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Disabled);
  fetcher.Stop();
  EXPECT_EQ(text_calls_, 0u);
  EXPECT_EQ(download_calls_, 0u);
  EXPECT_FALSE(std::filesystem::exists(Cache()));
}

TEST_F(TrendingWordFetcherTest, UnconfiguredSourceDoesNotContactNetwork) {
  TrendingWordFetcher fetcher(*store_, Cache(), L"", Dependencies());
  fetcher.UpdateSettings(true, 24, false);
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::SourceUnavailable);
  EXPECT_EQ(text_calls_, 0u);
  EXPECT_EQ(download_calls_, 0u);
}

TEST_F(TrendingWordFetcherTest, ShaMismatchKeepsStoreAndCacheAndNextAttemptSurvives) {
  SeedExisting();
  const auto before = store_->SerializeText();
  const auto correct = checksum_;
  checksum_ = std::string(64, '0');
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              Dependencies());
  fetcher.UpdateSettings(true, 24, false);
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Failed);
  EXPECT_EQ(store_->SerializeText(), before);
  EXPECT_EQ(ReadCache(), "old cache");
  checksum_ = correct;
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Ingested);
}

TEST_F(TrendingWordFetcherTest, InvalidChecksumNeverDownloadsAsset) {
  SeedExisting();
  const auto before = store_->SerializeText();
  checksum_ = "invalid";
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              Dependencies());
  fetcher.UpdateSettings(true, 24, false);
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Failed);
  EXPECT_EQ(download_calls_, 0u);
  EXPECT_EQ(store_->SerializeText(), before);
  EXPECT_EQ(ReadCache(), "old cache");
}

TEST_F(TrendingWordFetcherTest, ValidHashWithInvalidJsonPreservesStoreAndCache) {
  SeedExisting();
  const auto before = store_->SerializeText();
  for (const auto& invalid : {std::string("{"), std::string(R"({"version":2,"words":[]})"),
                              std::string(R"({"version":1,"generated_at":"date","words":[
                                {"surface":"新語","reading":"しんご","rank":0}]})")}) {
    SetAsset(invalid);
    TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                                Dependencies());
    fetcher.UpdateSettings(true, 24, false);
    EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Failed);
    EXPECT_EQ(store_->SerializeText(), before);
    EXPECT_EQ(ReadCache(), "old cache");
  }
}

TEST_F(TrendingWordFetcherTest, TransportFailureOrExceptionPreservesStoreAndCache) {
  SeedExisting();
  const auto before = store_->SerializeText();
  auto dependencies = Dependencies();
  dependencies.download = [](const HttpDownloadRequest&) { return HttpDownloadResult{}; };
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              dependencies);
  fetcher.UpdateSettings(true, 24, false);
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Failed);
  dependencies.fetch_text = [](const HttpTextRequest&) -> HttpTextResult {
    throw std::runtime_error("injected transport failure");
  };
  TrendingWordFetcher throwing(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                               dependencies);
  throwing.UpdateSettings(true, 24, false);
  EXPECT_EQ(throwing.FetchOnce(), TrendingFetchStatus::Failed);
  EXPECT_EQ(store_->SerializeText(), before);
  EXPECT_EQ(ReadCache(), "old cache");
}

TEST_F(TrendingWordFetcherTest, NonStringReadingRejectsWholeAsset) {
  SeedExisting();
  const auto before = store_->SerializeText();
  for (const std::string reading : {"123", "null", "{}", "[]", "true"}) {
    SetAsset(
        "{\"version\":1,\"generated_at\":\"date\",\"words\":["
        "{\"surface\":\"新語\",\"reading\":" +
        reading + ",\"rank\":1}]}");
    TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                                Dependencies());
    fetcher.UpdateSettings(true, 24, false);
    EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Failed);
    EXPECT_EQ(store_->SerializeText(), before);
    EXPECT_EQ(ReadCache(), "old cache");
  }
}

TEST_F(TrendingWordFetcherTest, WordByteLimitsAcceptBoundaryAndRejectOverflow) {
  SeedExisting();
  std::string reading;
  for (size_t i = 0; i < 85; ++i) reading += "あ";
  ASSERT_EQ(reading.size(), 255u);
  const std::vector<std::pair<std::string, std::string>> valid = {{std::string(256, 'a'), "しんご"},
                                                                  {"新語", reading}};
  for (const auto& [surface, ruby] : valid) {
    SCOPED_TRACE(surface.size());
    SetAsset(AssetWithWords(AssetWord(surface, ruby)));
    TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                                Dependencies());
    fetcher.UpdateSettings(true, 24, true);
    ASSERT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Ingested);
    const auto confirmed = store_->LookupConfirmed(ruby);
    ASSERT_EQ(confirmed.size(), 1u);
    EXPECT_EQ(confirmed.front().surface, surface);
  }
  ExpectRejectedAssetPreservesStoreAndCache(
      AssetWithWords(AssetWord(std::string(257, 'a'), "しんご")));
  ExpectRejectedAssetPreservesStoreAndCache(AssetWithWords(AssetWord("新語", reading + "あ")));
}

TEST_F(TrendingWordFetcherTest, ValidUnicodeSurfaceAndKanaMarksArePreserved) {
  const std::string surface = "漢字かなカナ🙂𠮷";
  std::string reading;
  for (char32_t cp : {0x3041, 0x309F, 0x30A0, 0x30FF, 0x3099, 0x309A, 0x30FC, 0x30FB}) {
    reading += Utf8Scalar(cp);
  }
  SetAsset(AssetWithWords(AssetWord(surface, reading)));
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              Dependencies());
  fetcher.UpdateSettings(true, 24, true);
  ASSERT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Ingested);
  const auto words = store_->LookupConfirmed(reading);
  ASSERT_EQ(words.size(), 1u);
  EXPECT_EQ(words.front().surface, surface);
  EXPECT_EQ(words.front().reading, reading);
}

TEST_F(TrendingWordFetcherTest, EveryControlAndBidiScalarRejectsBothWordFields) {
  SeedExisting();
  std::vector<char32_t> forbidden;
  for (char32_t cp = 0; cp <= 0x1F; ++cp) forbidden.push_back(cp);
  for (char32_t cp = 0x7F; cp <= 0x9F; ++cp) forbidden.push_back(cp);
  forbidden.push_back(0x061C);
  for (char32_t cp = 0x200E; cp <= 0x200F; ++cp) forbidden.push_back(cp);
  for (char32_t cp = 0x202A; cp <= 0x202E; ++cp) forbidden.push_back(cp);
  for (char32_t cp = 0x2066; cp <= 0x2069; ++cp) forbidden.push_back(cp);
  for (const char32_t cp : forbidden) {
    SCOPED_TRACE(static_cast<uint32_t>(cp));
    const auto scalar = Utf8Scalar(cp);
    // EscapeString makes C0 valid JSON escapes; rejection must happen after parsing.
    for (const bool in_reading : {false, true}) {
      SCOPED_TRACE(in_reading ? "reading" : "surface");
      ExpectRejectedAssetPreservesStoreAndCache(
          AssetWithWords(AssetWord(in_reading ? "新語" : "新" + scalar + "語",
                                   in_reading ? "し" + scalar + "んご" : "しんご")));
    }
  }
}

TEST_F(TrendingWordFetcherTest, InvalidUtf8AndNonKanaReadingsRejectWholeAsset) {
  SeedExisting();
  const std::vector<std::string> malformed = {
      std::string("\x80", 1),         std::string("\xE3\x81", 2),
      std::string("\xC0\xAF", 2),     std::string("\xE0\x80\x80", 3),
      std::string("\xED\xA0\x80", 3), std::string("\xF4\x90\x80\x80", 4)};
  for (size_t i = 0; i < malformed.size(); ++i) {
    SCOPED_TRACE(i);
    for (const bool in_reading : {false, true}) {
      SCOPED_TRACE(in_reading ? "reading" : "surface");
      ExpectRejectedAssetPreservesStoreAndCache(AssetWithWords(
          AssetWord(in_reading ? "新語" : malformed[i], in_reading ? malformed[i] : "しんご")));
    }
  }
  for (const std::string ruby : {"abc", "ｼﾝｺﾞ", "漢字", "しん ご", "しん1ご"}) {
    SCOPED_TRACE(ruby);
    ExpectRejectedAssetPreservesStoreAndCache(AssetWithWords(AssetWord("新語", ruby)));
  }
  for (char32_t cp : {0x3040, 0x3100}) {
    SCOPED_TRACE(static_cast<uint32_t>(cp));
    ExpectRejectedAssetPreservesStoreAndCache(AssetWithWords(AssetWord("新語", Utf8Scalar(cp))));
  }
  ExpectRejectedAssetPreservesStoreAndCache(
      AssetWithWords(R"({"surface":"\ud800","reading":"しんご","rank":1})"));
  ExpectRejectedAssetPreservesStoreAndCache(
      AssetWithWords(R"({"surface":"新語","reading":"\udc00","rank":1})"));
}

TEST_F(TrendingWordFetcherTest, WordCountLimitAcceptsTenThousandReadings) {
  SeedExisting();
  const auto at_limit = AssetWithWordCount(10000, false);
  ASSERT_LT(at_limit.size(), 1024u * 1024);
  SetAsset(at_limit);
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              Dependencies());
  fetcher.UpdateSettings(true, 24, true);
  ASSERT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Ingested);
  ASSERT_EQ(store_->LookupConfirmed("しんご").size(), 1u);
  EXPECT_EQ(store_->Size(), 2u);
}

TEST_F(TrendingWordFetcherTest, WordCountLimitRejectsTenThousandAndOneReadings) {
  SeedExisting();
  ExpectRejectedAssetPreservesStoreAndCache(AssetWithWordCount(10001, false));
}

TEST_F(TrendingWordFetcherTest, WordCountLimitIncludesMissingAndEmptyReadings) {
  SeedExisting();
  SetAsset(AssetWithWordCount(10000, true));
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              Dependencies());
  fetcher.UpdateSettings(true, 24, true);
  ASSERT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Ingested);
  ASSERT_EQ(store_->LookupConfirmed("しんご").size(), 1u);
  EXPECT_EQ(store_->Size(), 2u);
}

TEST_F(TrendingWordFetcherTest, WordCountLimitRejectsOverflowIncludingMissingAndEmptyReadings) {
  SeedExisting();
  ExpectRejectedAssetPreservesStoreAndCache(AssetWithWordCount(10001, true));
}

TEST_F(TrendingWordFetcherTest, DeadlineUsesInjectedClockAtAndAroundFifteenSeconds) {
  SeedExisting();
  for (const bool during_download : {false, true}) {
    SCOPED_TRACE(during_download ? "asset download" : "checksum fetch");
    for (const auto elapsed : {14999ms, 15000ms, 15001ms}) {
      SCOPED_TRACE(elapsed.count());
      const auto before = store_->SerializeText();
      const auto cache = ReadCache();
      const auto downloads_before = download_calls_.load();
      std::chrono::milliseconds clock{0};
      auto dependencies = Dependencies();
      dependencies.steady_now = [&] { return std::chrono::steady_clock::time_point{} + clock; };
      const auto fetch_text = dependencies.fetch_text;
      dependencies.fetch_text = [&](const HttpTextRequest& request) {
        auto result = fetch_text(request);
        if (!during_download) clock = elapsed;
        return result;
      };
      const auto download = dependencies.download;
      dependencies.download = [&](const HttpDownloadRequest& request) {
        auto result = download(request);
        if (during_download) clock = elapsed;
        return result;
      };
      TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                                  dependencies);
      fetcher.UpdateSettings(true, 24, false);
      if (elapsed < 15s) {
        EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Ingested);
        EXPECT_EQ(ReadCache(), kAsset);
      } else {
        EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Cancelled);
        EXPECT_EQ(store_->SerializeText(), before);
        EXPECT_EQ(ReadCache(), cache);
        AutoWordStore reloaded(store_->path());
        ASSERT_TRUE(reloaded.Load());
        EXPECT_EQ(reloaded.SerializeText(), before);
      }
      EXPECT_EQ(download_calls_.load() - downloads_before,
                during_download || elapsed < 15s ? 1u : 0u);
      EXPECT_FALSE(std::filesystem::exists(Cache().wstring() + L".download"));
      EXPECT_FALSE(std::filesystem::exists(Cache().wstring() + L".download.part"));
    }
  }
}

TEST_F(TrendingWordFetcherTest, InvalidSecondWordNeverPartiallyIngestsInAutoMode) {
  SeedExisting();
  const auto before = store_->SerializeText();
  ExpectRejectedAssetPreservesStoreAndCache(
      AssetWithWords(AssetWord("新語", "しんご") + ',' + AssetWord("汚染語", "not-kana")));
  EXPECT_TRUE(store_->ListByState(AutoWordState::Confirmed).empty());
  EXPECT_EQ(ReadCache(), "old cache");
  AutoWordStore reloaded(store_->path());
  ASSERT_TRUE(reloaded.Load());
  EXPECT_EQ(reloaded.SerializeText(), before);
}

TEST_F(TrendingWordFetcherTest, FailedStoreSaveKeepsValidMemoryAndRetriesUnchangedGeneration) {
  auto encrypted_path = store_->path();
  encrypted_path += ".enc";
  ASSERT_TRUE(std::filesystem::create_directory(encrypted_path));
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              Dependencies());
  fetcher.UpdateSettings(true, 24, false);
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Failed);
  EXPECT_EQ(ReadCache(), kAsset);
  EXPECT_EQ(store_->Size(), 2u);
  ASSERT_TRUE(std::filesystem::remove(encrypted_path));
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Ingested);
  AutoWordStore reloaded(store_->path());
  ASSERT_TRUE(reloaded.Load());
  EXPECT_EQ(reloaded.SerializeText(), store_->SerializeText());
}

TEST_F(TrendingWordFetcherTest, UnchangedGenerationDoesNotRefreshRecency) {
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              Dependencies());
  fetcher.UpdateSettings(true, 24, false);
  ASSERT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Ingested);
  const auto before = store_->SerializeText();
  epoch_ = 1000;
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Unchanged);
  EXPECT_EQ(store_->SerializeText(), before);
}

TEST_F(TrendingWordFetcherTest, AutoModePreservesRejectedAndMiningPrecedence) {
  store_->Observe("推し活", "おしかつ", 10, 3, false);
  store_->Observe("新語", "しんご", 10, 3, false);
  ASSERT_TRUE(store_->Reject("新語", "しんご"));
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              Dependencies());
  fetcher.UpdateSettings(true, 24, true);
  ASSERT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Ingested);
  const auto confirmed = store_->LookupConfirmed("おしかつ");
  ASSERT_EQ(confirmed.size(), 1u);
  EXPECT_EQ(confirmed[0].source, AutoWordSource::Mining);
  EXPECT_EQ(store_->ListByState(AutoWordState::Rejected).size(), 1u);
}

TEST_F(TrendingWordFetcherTest, DisableDuringDownloadDiscardsObsoleteResultAndCanReenable) {
  SeedExisting();
  const auto before = store_->SerializeText();
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  auto dependencies = Dependencies();
  const auto download = dependencies.download;
  dependencies.download = [&](const HttpDownloadRequest& request) {
    entered.set_value();
    release_future.wait();
    EXPECT_TRUE(request.cancelled());
    return download(request);
  };
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              dependencies);
  fetcher.UpdateSettings(true, 24, false);
  auto result = std::async(std::launch::async, [&] { return fetcher.FetchOnce(); });
  const auto ready = entered.get_future().wait_for(5s);
  // Release even after a failed assertion, so the test never leaves a blocked worker.
  fetcher.UpdateSettings(false, 24, false);
  release.set_value();
  EXPECT_EQ(ready, std::future_status::ready);
  EXPECT_EQ(result.get(), TrendingFetchStatus::Cancelled);
  EXPECT_EQ(fetcher.FetchOnce(), TrendingFetchStatus::Disabled);
  EXPECT_EQ(store_->SerializeText(), before);
  EXPECT_EQ(ReadCache(), "old cache");
  TrendingWordFetcher reenabled(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                                Dependencies());
  reenabled.UpdateSettings(true, 1, false);
  EXPECT_EQ(reenabled.FetchOnce(), TrendingFetchStatus::Ingested);
}

TEST_F(TrendingWordFetcherTest, WorkerFetchesAtStartAndStopInterruptsIntervalWait) {
  std::promise<TrendingFetchStatus> completed;
  auto dependencies = Dependencies();
  dependencies.report = [&](TrendingFetchStatus status) { completed.set_value(status); };
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              dependencies);
  fetcher.UpdateSettings(true, 8760, false);
  fetcher.Start();
  auto result = completed.get_future();
  EXPECT_EQ(result.wait_for(5s), std::future_status::ready);
  fetcher.Stop();
  ASSERT_EQ(result.wait_for(0s), std::future_status::ready);
  EXPECT_EQ(result.get(), TrendingFetchStatus::Ingested);
  EXPECT_EQ(text_calls_, 1u);
  EXPECT_EQ(download_calls_, 1u);
}

TEST_F(TrendingWordFetcherTest, StopCancelsInflightWorkerAndJoinsBeforeReturning) {
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::atomic<bool> observed_cancel{false};
  auto dependencies = Dependencies();
  dependencies.fetch_text = [&](const HttpTextRequest& request) {
    entered.set_value();
    release_future.wait();
    while (!request.cancelled()) std::this_thread::yield();
    observed_cancel = true;
    return HttpTextResult{{}, "cancelled"};
  };
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              dependencies);
  fetcher.UpdateSettings(true, 24, false);
  fetcher.Start();
  const auto ready = entered.get_future().wait_for(5s);
  auto stop = std::async(std::launch::async, [&] { fetcher.Stop(); });
  release.set_value();
  EXPECT_EQ(ready, std::future_status::ready);
  EXPECT_EQ(stop.wait_for(5s), std::future_status::ready);
  stop.get();
  EXPECT_TRUE(observed_cancel);
  EXPECT_EQ(download_calls_, 0u);
  EXPECT_EQ(store_->Size(), 0u);
}

TEST_F(TrendingWordFetcherTest, WorkerRepeatsAtConfiguredIntervalUsingVirtualTime) {
  std::atomic<int> hours{0};
  std::promise<void> first;
  std::promise<void> second;
  unsigned reports = 0;
  auto dependencies = Dependencies();
  dependencies.steady_now = [&] {
    return std::chrono::steady_clock::time_point{} + std::chrono::hours(hours.load());
  };
  dependencies.report = [&](TrendingFetchStatus status) {
    if (++reports == 1) {
      EXPECT_EQ(status, TrendingFetchStatus::Ingested);
      first.set_value();
    } else if (reports == 2) {
      EXPECT_EQ(status, TrendingFetchStatus::Unchanged);
      second.set_value();
    }
  };
  TrendingWordFetcher fetcher(*store_, Cache(), L"https://fixture.invalid/trending-words.json",
                              dependencies);
  fetcher.UpdateSettings(true, 24, false);
  fetcher.Start();
  EXPECT_EQ(first.get_future().wait_for(5s), std::future_status::ready);
  hours = 24;
  // Re-publishing identical settings wakes the virtual clock without starting
  // a new settings generation or cancelling the scheduled interval.
  fetcher.UpdateSettings(true, 24, false);
  EXPECT_EQ(second.get_future().wait_for(5s), std::future_status::ready);
  fetcher.Stop();
  EXPECT_EQ(text_calls_, 2u);
  EXPECT_EQ(download_calls_, 2u);
}

}  // namespace
