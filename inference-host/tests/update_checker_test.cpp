#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "azookey/host/UpdateChecker.h"
#include "azookey/ipc/Json.h"

namespace {
using namespace std::chrono_literals;
using azookey::host::AvailableRelease;
using azookey::host::CompareReleaseVersions;
using azookey::host::HttpTextRequest;
using azookey::host::HttpTextResult;
using azookey::host::ParseReleaseVersion;
using azookey::host::SelectRelease;
using azookey::host::UpdateCheckDependencies;
using azookey::host::UpdateChecker;
using azookey::host::UpdateCheckerOptions;
using azookey::host::UpdateCheckStatus;

constexpr std::string_view kPrefix = "https://github.com/dolquis/azooKey-Desktop/releases/";
constexpr uint64_t kNow = 1'800'000'000;

std::string Release(std::string_view tag, bool prerelease = false, bool draft = false,
                    std::string_view url = {}) {
  const std::string page =
      url.empty() ? std::string(kPrefix) + "tag/" + std::string(tag) : std::string(url);
  return R"({"tag_name":")" + std::string(tag) + R"(","html_url":")" + page + R"(","prerelease":)" +
         (prerelease ? "true" : "false") + R"(,"draft":)" + (draft ? "true" : "false") +
         R"(,"body":"notes"})";
}

class TempDirectory {
 public:
  TempDirectory() {
    static std::atomic<uint64_t> next{0};
    path_ = std::filesystem::temp_directory_path() /
            ("azookey-update-checker-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
             std::to_string(next.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::optional<azookey::ipc::json::Value> ReadStatus(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  return azookey::ipc::json::Parse(text);
}

void WriteStatus(const std::filesystem::path& path, uint64_t checked_at, std::string_view channel) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << R"({"version":1,"checkedAtEpoch":)" << checked_at << R"(,"channel":")" << channel
         << R"(","result":"up_to_date","currentVersion":"1.0.0"})";
}

// A fetch stub that records requests and answers with a fixed result.
struct FakeReleases {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<HttpTextRequest> requests;
  HttpTextResult answer;
  std::function<void()> during_fetch;

  HttpTextResult Fetch(const HttpTextRequest& request) {
    if (during_fetch) during_fetch();
    std::lock_guard lock(mutex);
    requests.push_back(request);
    changed.notify_all();
    return answer;
  }
  size_t count() {
    std::lock_guard lock(mutex);
    return requests.size();
  }
  // Bounded far above the time a worker needs; the bound only keeps a failure from hanging.
  bool WaitForCount(size_t expected) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 30s, [&] { return requests.size() >= expected; });
  }
};

// A steady clock the test moves by hand; also counts how often the worker reads it.
struct FakeSteadyClock {
  std::atomic<int64_t> offset_seconds{0};
  std::atomic<uint64_t> reads{0};
  std::chrono::steady_clock::time_point Now() {
    ++reads;
    return std::chrono::steady_clock::time_point{} + std::chrono::seconds(offset_seconds.load());
  }
  // True once the worker has read the clock `count` more times than `since`.
  bool WaitForReads(uint64_t since, uint64_t count) {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (reads.load() < since + count) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::yield();
    }
    return true;
  }
};

struct Fixture {
  TempDirectory temp;
  FakeReleases releases;
  FakeSteadyClock clock;
  std::filesystem::path status_path = temp.path() / "update-status.json";
  // The published privacy policy as main.cpp passes it (false: secure or invalid settings).
  std::atomic<bool> network_allowed{true};

  UpdateChecker Make(std::string current_version = "1.0.0") {
    UpdateCheckerOptions options;
    options.current_version = std::move(current_version);
    options.api_base_url = L"https://api.example.test/repos/owner/repo";
    options.release_page_prefix = std::string(kPrefix);
    options.status_path = status_path;
    UpdateCheckDependencies dependencies;
    dependencies.fetch_text = [this](const HttpTextRequest& request) {
      return releases.Fetch(request);
    };
    dependencies.now_epoch = [] { return kNow; };
    dependencies.steady_now = [this] { return clock.Now(); };
    dependencies.network_allowed = [this] { return network_allowed.load(); };
    return UpdateChecker(std::move(options), std::move(dependencies));
  }
};

TEST(UpdateCheckerVersionTest, ParsesTagsWithAndWithoutThePrefix) {
  const auto plain = ParseReleaseVersion("1.2.3");
  ASSERT_TRUE(plain);
  EXPECT_EQ(plain->major, 1u);
  EXPECT_EQ(plain->minor, 2u);
  EXPECT_EQ(plain->patch, 3u);
  const auto tagged = ParseReleaseVersion("v10.0.1-beta.2");
  ASSERT_TRUE(tagged);
  EXPECT_EQ(tagged->major, 10u);
  EXPECT_EQ(tagged->prerelease, (std::vector<std::string>{"beta", "2"}));
  for (const char* invalid : {"", "v", "1.2", "1.2.3.4", "01.2.3", "1.2.-3", "1.2.3-", "1.2.3-a..b",
                              "1.2.3-01", "V1.2.3", "1.2.3 ", "nightly", "1.2.x",
                              // Build metadata would be shown without being compared.
                              "1.2.3+build.5", "v1.2.3-beta.1+sha"}) {
    EXPECT_FALSE(ParseReleaseVersion(invalid)) << invalid;
  }
}

TEST(UpdateCheckerVersionTest, FollowsSemanticVersionPrecedence) {
  // The ordering example of SemVer 2.0.0 section 11, then a higher patch.
  const std::vector<std::string> ordered = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta",
                                            "1.0.0-beta",  "1.0.0-beta.2",  "1.0.0-beta.11",
                                            "1.0.0-rc.1",  "1.0.0",         "1.0.1",
                                            "1.10.0",      "2.0.0"};
  for (size_t i = 0; i + 1 < ordered.size(); ++i) {
    const auto lower = ParseReleaseVersion(ordered[i]);
    const auto higher = ParseReleaseVersion(ordered[i + 1]);
    ASSERT_TRUE(lower && higher) << ordered[i];
    EXPECT_LT(CompareReleaseVersions(*lower, *higher), 0) << ordered[i] << " < " << ordered[i + 1];
    EXPECT_GT(CompareReleaseVersions(*higher, *lower), 0);
  }
  EXPECT_EQ(CompareReleaseVersions(*ParseReleaseVersion("v1.0.0"), *ParseReleaseVersion("1.0.0")),
            0);
}

TEST(UpdateCheckerSelectTest, StableTakesTheLatestReleaseObjectOnly) {
  const auto release = SelectRelease(Release("v1.2.0"), "stable", kPrefix);
  ASSERT_TRUE(release);
  EXPECT_EQ(release->version, "1.2.0");
  EXPECT_EQ(release->url, std::string(kPrefix) + "tag/v1.2.0");
  EXPECT_FALSE(SelectRelease(Release("v1.2.0", true), "stable", kPrefix));
  EXPECT_FALSE(SelectRelease(Release("v1.2.0", false, true), "stable", kPrefix));
  EXPECT_FALSE(SelectRelease("[" + Release("v1.2.0") + "]", "stable", kPrefix));
  EXPECT_FALSE(SelectRelease(Release("nightly"), "stable", kPrefix));
  EXPECT_FALSE(SelectRelease("{not json", "stable", kPrefix));
}

TEST(UpdateCheckerSelectTest, BetaTakesTheHighestNonDraftReleaseIncludingPrereleases) {
  const std::string list = "[" + Release("v1.1.0") + "," + Release("v1.3.0", false, true) + "," +
                           Release("v1.2.0-beta.1", true) + "," + Release("garbage") + "," +
                           Release("v1.2.0-beta.10", true) + "]";
  const auto release = SelectRelease(list, "beta", kPrefix);
  ASSERT_TRUE(release);
  EXPECT_EQ(release->version, "1.2.0-beta.10");
  EXPECT_FALSE(SelectRelease(Release("v1.1.0"), "beta", kPrefix));
  EXPECT_FALSE(SelectRelease("[]", "beta", kPrefix));
}

TEST(UpdateCheckerSelectTest, KeepsOnlyReleasePagesOfTheRepository) {
  for (const char* url : {"https://example.com/releases/tag/v1.2.0",
                          "http://github.com/dolquis/azooKey-Desktop/releases/tag/v1.2.0",
                          "https://github.com/dolquis/azooKey-Desktop/releases/",
                          "https://github.com/dolquis/azooKey-Desktop/releases/tag/v1 2",
                          "https://github.com/dolquis/azooKey-Desktop/releases/../../other/repo",
                          "https://github.com/dolquis/azooKey-Desktop/releases/tag/%2e%2e"}) {
    const auto release = SelectRelease(Release("v1.2.0", false, false, url), "stable", kPrefix);
    ASSERT_TRUE(release) << url;
    EXPECT_EQ(release->version, "1.2.0");
    EXPECT_TRUE(release->url.empty()) << url;
  }
}

TEST(UpdateCheckerTest, DisabledNeverReachesTheNetworkOrWritesStatus) {
  Fixture f;
  auto checker = f.Make();
  EXPECT_EQ(checker.CheckOnce(), UpdateCheckStatus::Disabled);
  EXPECT_EQ(f.releases.count(), 0u);
  EXPECT_FALSE(std::filesystem::exists(f.status_path));
}

TEST(UpdateCheckerTest, NewerStableReleaseIsRecordedForTheSettingsApp) {
  Fixture f;
  f.releases.answer.body = Release("v1.1.0");
  auto checker = f.Make("1.0.0");
  checker.UpdateSettings(true, "stable", 24);
  EXPECT_EQ(checker.CheckOnce(), UpdateCheckStatus::UpdateAvailable);
  ASSERT_EQ(f.releases.count(), 1u);
  const auto& request = f.releases.requests.front();
  EXPECT_EQ(request.url, L"https://api.example.test/repos/owner/repo/releases/latest");
  EXPECT_GT(request.max_bytes, 0u);
  ASSERT_TRUE(request.cancelled);
  EXPECT_FALSE(request.cancelled());

  const auto status = ReadStatus(f.status_path);
  ASSERT_TRUE(status && status->IsObject());
  EXPECT_EQ(status->GetUInt("version"), 1u);
  EXPECT_EQ(status->GetUInt("checkedAtEpoch"), kNow);
  EXPECT_EQ(status->GetString("channel"), "stable");
  EXPECT_EQ(status->GetString("result"), "update_available");
  EXPECT_EQ(status->GetString("currentVersion"), "1.0.0");
  EXPECT_EQ(status->GetString("latestVersion"), "1.1.0");
  EXPECT_EQ(status->GetString("releaseUrl"), std::string(kPrefix) + "tag/v1.1.0");
}

TEST(UpdateCheckerTest, SameOrOlderReleaseIsUpToDate) {
  for (const char* tag : {"v1.0.0", "v0.9.0", "v1.0.0-rc.1"}) {
    Fixture f;
    f.releases.answer.body = Release(tag);
    auto checker = f.Make("1.0.0");
    checker.UpdateSettings(true, "stable", 24);
    EXPECT_EQ(checker.CheckOnce(), UpdateCheckStatus::UpToDate) << tag;
    const auto status = ReadStatus(f.status_path);
    ASSERT_TRUE(status) << tag;
    EXPECT_EQ(status->GetString("result"), "up_to_date");
  }
}

TEST(UpdateCheckerTest, BetaChannelAsksForTheReleaseList) {
  Fixture f;
  f.releases.answer.body = "[" + Release("v1.0.0") + "," + Release("v1.1.0-beta.1", true) + "]";
  auto checker = f.Make("1.0.0");
  checker.UpdateSettings(true, "beta", 24);
  EXPECT_EQ(checker.CheckOnce(), UpdateCheckStatus::UpdateAvailable);
  ASSERT_EQ(f.releases.count(), 1u);
  EXPECT_EQ(f.releases.requests.front().url,
            L"https://api.example.test/repos/owner/repo/releases?per_page=20");
  EXPECT_EQ(ReadStatus(f.status_path)->GetString("latestVersion"), "1.1.0-beta.1");
}

TEST(UpdateCheckerTest, FailuresAreRecordedWithoutAVersion) {
  for (const bool transport_failure : {true, false}) {
    Fixture f;
    if (transport_failure) {
      f.releases.answer.error = "receiving HTTP response failed (Windows error 12002)";
    } else {
      f.releases.answer.body = R"({"message":"API rate limit exceeded"})";
    }
    auto checker = f.Make();
    checker.UpdateSettings(true, "stable", 24);
    EXPECT_EQ(checker.CheckOnce(), UpdateCheckStatus::Failed);
    const auto status = ReadStatus(f.status_path);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->GetString("result"), "failed");
    EXPECT_FALSE(status->Find("latestVersion"));
    EXPECT_FALSE(status->Find("releaseUrl"));
  }
}

TEST(UpdateCheckerTest, UnparsableProductVersionFailsBeforeTheNetwork) {
  Fixture f;
  auto checker = f.Make("dev");
  checker.UpdateSettings(true, "stable", 24);
  EXPECT_EQ(checker.CheckOnce(), UpdateCheckStatus::Failed);
  EXPECT_EQ(f.releases.count(), 0u);
}

TEST(UpdateCheckerTest, SettingsChangeDuringACheckCancelsItWithoutWritingStatus) {
  Fixture f;
  f.releases.answer.body = Release("v2.0.0");
  auto checker = f.Make();
  checker.UpdateSettings(true, "stable", 24);
  f.releases.during_fetch = [&] { checker.UpdateSettings(false, "stable", 24); };
  EXPECT_EQ(checker.CheckOnce(), UpdateCheckStatus::Cancelled);
  ASSERT_EQ(f.releases.count(), 1u);
  EXPECT_TRUE(f.releases.requests.front().cancelled());
  EXPECT_FALSE(std::filesystem::exists(f.status_path));
}

TEST(UpdateCheckerTest, APrivacyPolicyThatForbidsTheNetworkHoldsTheCheckBack) {
  // An invalid settings.json applies no config, so the opt-in stays on; the published policy

  // (secure, fail closed) still keeps the check off the network.

  Fixture f;

  f.releases.answer.body = Release("v2.0.0");

  f.network_allowed = false;

  auto checker = f.Make();

  checker.UpdateSettings(true, "stable", 24);

  EXPECT_EQ(checker.CheckOnce(), UpdateCheckStatus::Disabled);

  EXPECT_EQ(f.releases.count(), 0u);

  EXPECT_FALSE(std::filesystem::exists(f.status_path));

  // A policy that turns secure during a check cancels it without a record.

  f.network_allowed = true;

  f.releases.during_fetch = [&] { f.network_allowed = false; };

  EXPECT_EQ(checker.CheckOnce(), UpdateCheckStatus::Cancelled);

  EXPECT_EQ(f.releases.count(), 1u);

  EXPECT_FALSE(std::filesystem::exists(f.status_path));
}

TEST(UpdateCheckerWorkerTest, AHeldBackWorkerWaitsForTheNextReloadWithoutSpinning) {
  Fixture f;

  f.releases.answer.body = Release("v1.0.0");

  f.network_allowed = false;

  auto checker = f.Make();

  checker.UpdateSettings(true, "stable", 24);

  checker.Start();

  // Clock reads: the due time and its remaining wait; then the worker finds the check held

  // back and waits for a reload. A spinning worker would keep reading the clock.

  ASSERT_TRUE(f.clock.WaitForReads(0, 2));

  const auto reads = f.clock.reads.load();

  std::this_thread::sleep_for(50ms);

  EXPECT_EQ(f.clock.reads.load(), reads);

  EXPECT_EQ(f.releases.count(), 0u);

  // A valid reload that leaves the settings as they were lets it check.

  f.network_allowed = true;

  checker.UpdateSettings(true, "stable", 24);

  EXPECT_TRUE(f.releases.WaitForCount(1));

  checker.Stop();
}

TEST(UpdateCheckerWorkerTest, ChecksAtStartWhenNothingIsRecorded) {
  Fixture f;
  f.releases.answer.body = Release("v1.0.0");
  auto checker = f.Make();
  checker.UpdateSettings(true, "stable", 24);
  checker.Start();
  EXPECT_TRUE(f.releases.WaitForCount(1));
  checker.Stop();
  EXPECT_EQ(f.releases.count(), 1u);
}

TEST(UpdateCheckerWorkerTest, ARecordedCheckDefersTheNextUntilTheIntervalPasses) {
  Fixture f;
  f.releases.answer.body = Release("v1.0.0");
  // An earlier Host process checked this channel an hour ago.
  WriteStatus(f.status_path, kNow - 3600, "stable");
  auto checker = f.Make();
  checker.UpdateSettings(true, "stable", 24);
  const auto before = f.clock.reads.load();
  checker.Start();
  // Two reads: computing the due time, then the remaining wait the worker sleeps on.
  ASSERT_TRUE(f.clock.WaitForReads(before, 2));
  EXPECT_EQ(f.releases.count(), 0u);

  f.clock.offset_seconds += 23 * 3600;
  checker.UpdateSettings(true, "stable", 24);  // Unchanged settings only wake the worker.
  EXPECT_TRUE(f.releases.WaitForCount(1));
  checker.Stop();
}

TEST(UpdateCheckerWorkerTest, ARecordForTheOtherChannelDoesNotDefer) {
  Fixture f;
  f.releases.answer.body = "[" + Release("v1.0.0") + "]";
  WriteStatus(f.status_path, kNow - 60, "stable");
  auto checker = f.Make();
  checker.UpdateSettings(true, "beta", 24);
  checker.Start();
  EXPECT_TRUE(f.releases.WaitForCount(1));
  checker.Stop();
}

TEST(UpdateCheckerWorkerTest, ARecordFromAnotherBuildDoesNotDefer) {
  // 1.0.0 recorded an available 1.1.0 a minute ago; 1.1.0 is now installed and must not keep
  // showing that record until the interval passes.
  Fixture f;
  f.releases.answer.body = Release("v1.1.0");
  WriteStatus(f.status_path, kNow - 60, "stable");
  auto checker = f.Make("1.1.0");
  checker.UpdateSettings(true, "stable", 24);
  checker.Start();
  EXPECT_TRUE(f.releases.WaitForCount(1));
  checker.Stop();
  const auto status = ReadStatus(f.status_path);
  ASSERT_TRUE(status);
  EXPECT_EQ(status->GetString("result"), "up_to_date");
}

TEST(UpdateCheckerWorkerTest, AFailedCheckIsRetriedAtTheNextIntervalNotAtOnce) {
  Fixture f;
  f.releases.answer.error = "network";
  auto checker = f.Make();
  checker.UpdateSettings(true, "stable", 6);
  checker.Start();
  ASSERT_TRUE(f.releases.WaitForCount(1));
  // Clock reads: the first due time, its remaining wait, the attempt time, the second due
  // time, and then the remaining wait before the retry, which the worker sleeps on.
  ASSERT_TRUE(f.clock.WaitForReads(0, 5));
  EXPECT_EQ(f.releases.count(), 1u);

  f.clock.offset_seconds += 6 * 3600;
  checker.UpdateSettings(true, "stable", 6);
  EXPECT_TRUE(f.releases.WaitForCount(2));
  checker.Stop();
}

TEST(UpdateCheckerWorkerTest, DisabledWorkerDoesNotCheckAndStopReturns) {
  Fixture f;
  auto checker = f.Make();
  checker.Start();
  checker.UpdateSettings(false, "stable", 24);
  checker.Stop();
  EXPECT_EQ(f.releases.count(), 0u);
}

}  // namespace
