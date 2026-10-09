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

#include "azookey/host/TrendingWordFetcher.h"
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
