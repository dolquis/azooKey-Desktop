#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "azookey/core/PlatformPaths.h"
#include "azookey/host/CatalogFetchService.h"
#include "azookey/host/CatalogModelMatcher.h"
#include "azookey/host/ModelCatalog.h"

namespace {

namespace fs = std::filesystem;
using azookey::host::CatalogEntryFileState;
using azookey::host::CatalogFetchSnapshot;
using azookey::host::CatalogFetchStartResult;
using azookey::host::HttpDownloadRequest;
using azookey::host::HttpDownloadResult;
using azookey::host::HttpDownloadStatus;
using azookey::host::ModelFetchStatus;
using azookey::host::ZenzaiPromptFormat;

constexpr std::string_view kShaA =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kShaB =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

// Per-test directory: CTest runs each test in its own process, possibly in parallel.
fs::path TestDir() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  auto path = fs::temp_directory_path() / (std::string("azookey_catalog_fetch_") +
                                           info->test_suite_name() + "_" + info->name());
  fs::remove_all(path);
  fs::create_directories(path);
  return path;
}

void AppendLe(std::string& out, uint64_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

// A GGUF v3 header that passes the section 3.3 checks; no tensor data.
std::string ValidGgufBytes() {
  std::string out = "GGUF";
  AppendLe(out, 3, 4);
  AppendLe(out, 1, 8);
  AppendLe(out, 1, 8);
  const std::string key = "general.architecture";
  AppendLe(out, key.size(), 8);
  out += key;
  AppendLe(out, 8, 4);
  const std::string value = "gpt2";
  AppendLe(out, value.size(), 8);
  out += value;
  return out;
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

azookey::host::ModelCatalogEntry MakeEntry(std::string id, std::string file_name,
                                           std::string_view sha256, ZenzaiPromptFormat format) {
  azookey::host::ModelCatalogEntry entry;
  entry.id = std::move(id);
  entry.display_name = entry.id;
  entry.file_name = std::move(file_name);
  entry.sha256 = std::string(sha256);
  entry.prompt_format = format;
  entry.size_bytes = 4096;
  entry.source =
      azookey::host::ModelCatalogSource{"owner/repo", "0123456789abcdef0123456789abcdef01234567"};
  return entry;
}

azookey::host::ModelCatalog TwoEntryCatalog() {
  azookey::host::ModelCatalog catalog;
  catalog.schema_version = 2;
  catalog.default_model_id = "zenz";
  catalog.models = {MakeEntry("zenz", "zenz.gguf", kShaA, ZenzaiPromptFormat::ZenzV3),
                    MakeEntry("edge", "edge.gguf", kShaB, ZenzaiPromptFormat::KanakanjiEdgeV1)};
  return catalog;
}

// The SHA-256 of a file is its first line, so a test chooses it by writing the file; each call
// is counted.
struct CountingSha256 {
  std::shared_ptr<int> calls = std::make_shared<int>(0);

  azookey::host::FileSha256Function Function() const {
    return [calls = calls](const fs::path& path) -> std::optional<std::string> {
      ++*calls;
      std::ifstream in(path, std::ios::binary);
      std::string line;
      if (!std::getline(in, line)) return std::nullopt;
      return line;
    };
  }
};

std::string Utf8(const fs::path& path) { return azookey::core::PathToUtf8(path); }

}  // namespace

TEST(CatalogModelMatcherTest, NormalizesTheSpellingsMatchSelectedPathWouldNotCompare) {
  EXPECT_EQ(azookey::host::NormalizeSelectedModelPath(R"(\\?\C:\m\zenzai\zenz.gguf)"),
            fs::path(R"(C:\m\zenzai\zenz.gguf)").lexically_normal());
  EXPECT_EQ(azookey::host::NormalizeSelectedModelPath(R"(C:\m\zenzai\zenz.gguf\)"),
            fs::path(R"(C:\m\zenzai\zenz.gguf)").lexically_normal());
  EXPECT_EQ(azookey::host::NormalizeSelectedModelPath(R"(\\?\UNC\server\share\zenz.gguf)"),
            fs::path(R"(\\server\share\zenz.gguf)").lexically_normal());
}

TEST(CatalogModelMatcherTest, MatchesByPlaceNameAndShaAndTakesTheEntryFormat) {
  const auto dir = TestDir();
  const auto zenzai = dir / "zenzai";
  WriteBytes(zenzai / "EDGE.gguf", std::string(kShaB) + "\n");
  CountingSha256 sha;
  azookey::host::CatalogModelMatcher matcher(TwoEntryCatalog(), zenzai, sha.Function());

  const auto format = matcher.PromptFormatFor(Utf8(zenzai / "EDGE.gguf"));
  ASSERT_TRUE(format.has_value());
  EXPECT_EQ(*format, ZenzaiPromptFormat::KanakanjiEdgeV1);
  EXPECT_EQ(matcher.Match(Utf8(zenzai / "EDGE.gguf"))->id, "edge");
  // The second call reuses the hash: size and write time did not change.
  EXPECT_EQ(*sha.calls, 1);
  fs::remove_all(dir);
}

TEST(CatalogModelMatcherTest, NeverHashesAFileNoEntryNames) {
  const auto dir = TestDir();
  const auto zenzai = dir / "zenzai";
  WriteBytes(zenzai / "mine.gguf", std::string(kShaA) + "\n");
  WriteBytes(dir / "zenz.gguf", std::string(kShaA) + "\n");
  CountingSha256 sha;
  azookey::host::CatalogModelMatcher matcher(TwoEntryCatalog(), zenzai, sha.Function());

  EXPECT_FALSE(matcher.PromptFormatFor(Utf8(zenzai / "mine.gguf")));
  // The right name outside zenzai\ is not the catalog's file either.
  EXPECT_FALSE(matcher.PromptFormatFor(Utf8(dir / "zenz.gguf")));
  EXPECT_FALSE(matcher.PromptFormatFor(""));
  EXPECT_EQ(*sha.calls, 0);
  fs::remove_all(dir);
}

TEST(CatalogModelMatcherTest, AnotherShaUnderTheEntryNameDoesNotMatch) {
  const auto dir = TestDir();
  const auto zenzai = dir / "zenzai";
  WriteBytes(zenzai / "zenz.gguf", std::string(kShaB) + "\n");
  CountingSha256 sha;
  azookey::host::CatalogModelMatcher matcher(TwoEntryCatalog(), zenzai, sha.Function());

  EXPECT_FALSE(matcher.PromptFormatFor(Utf8(zenzai / "zenz.gguf")));
  EXPECT_EQ(matcher.StateOf(matcher.catalog().models[0]), CatalogEntryFileState::Mismatch);
  EXPECT_EQ(matcher.StateOf(matcher.catalog().models[1]), CatalogEntryFileState::Missing);
  fs::remove_all(dir);
}

TEST(CatalogModelMatcherTest, HashesAgainWhenTheFileSizeChanges) {
  const auto dir = TestDir();
  const auto zenzai = dir / "zenzai";
  const auto path = zenzai / "zenz.gguf";
  WriteBytes(path, std::string(kShaB) + "\n");
  CountingSha256 sha;
  azookey::host::CatalogModelMatcher matcher(TwoEntryCatalog(), zenzai, sha.Function());
  EXPECT_EQ(matcher.StateOf(matcher.catalog().models[0]), CatalogEntryFileState::Mismatch);

  // Replaced by the pinned file: a different size, so the kept hash is not used.
  WriteBytes(path, std::string(kShaA) + "\nmore bytes");
  EXPECT_EQ(matcher.StateOf(matcher.catalog().models[0]), CatalogEntryFileState::Verified);
  EXPECT_EQ(matcher.PromptFormatFor(Utf8(path)), ZenzaiPromptFormat::ZenzV3);
  EXPECT_EQ(*sha.calls, 2);
  fs::remove_all(dir);
}

TEST(CatalogModelMatcherTest, ResolverIsEmptyWithoutAMatcherAndNulloptWithoutAMatch) {
  EXPECT_FALSE(azookey::host::CatalogPromptFormatResolver(nullptr, "C:/m/zenzai/zenz.gguf"));
  const auto dir = TestDir();
  auto matcher = std::make_shared<azookey::host::CatalogModelMatcher>(
      TwoEntryCatalog(), dir / "zenzai", CountingSha256{}.Function());
  EXPECT_FALSE(azookey::host::CatalogPromptFormatResolver(matcher, ""));
  const auto resolver =
      azookey::host::CatalogPromptFormatResolver(matcher, Utf8(dir / "zenzai" / "zenz.gguf"));
  ASSERT_TRUE(resolver);
  EXPECT_FALSE(resolver().has_value());
  fs::remove_all(dir);
}

TEST(CatalogModelMatcherTest, DefaultMatcherReadsTheEmbeddedCatalog) {
  EXPECT_EQ(azookey::host::MakeDefaultCatalogModelMatcher({}), nullptr);
  const auto matcher = azookey::host::MakeDefaultCatalogModelMatcher("C:/m");
  ASSERT_NE(matcher, nullptr);
  EXPECT_FALSE(matcher->catalog().models.empty());
  EXPECT_EQ(matcher->zenzai_dir(), azookey::host::DefaultZenzaiModelDirectory("C:/m"));
}

namespace {

// Holds each download until the test releases it, reporting progress first, so a test can
// look at a running fetch. Writes the body only when served, like HttpDownloader.
class GatedDownloader final : public azookey::host::IPackDownloader {
 public:
  struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{false};
    bool released{false};
    bool saw_network_allowed{false};
    std::vector<HttpDownloadRequest> requests;
  };

  explicit GatedDownloader(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}

  HttpDownloadResult Download(const HttpDownloadRequest& request) override {
    HttpDownloadResult result;
    {
      std::lock_guard<std::mutex> lock(gate_->mutex);
      gate_->requests.push_back(request);
    }
    if (request.network_allowed && !request.network_allowed()) {
      result.error = "network disabled";
      return result;
    }
    if (request.progress) request.progress(1024, 0);
    {
      std::unique_lock<std::mutex> lock(gate_->mutex);
      gate_->entered = true;
      gate_->cv.notify_all();
      // Polls the cancellation like HttpDownloader does between reads; nothing notifies it.
      while (!gate_->released && !(request.cancelled && request.cancelled())) {
        gate_->cv.wait_for(lock, std::chrono::milliseconds(5));
      }
    }
    if (request.cancelled && request.cancelled()) {
      result.error = "HTTP request cancelled";
      return result;
    }
    WriteBytes(request.destination, ValidGgufBytes());
    result.bytes_received = 4096;
    result.status = HttpDownloadStatus::Downloaded;
    return result;
  }

 private:
  std::shared_ptr<Gate> gate_;
};

void WaitEntered(GatedDownloader::Gate& gate) {
  std::unique_lock<std::mutex> lock(gate.mutex);
  ASSERT_TRUE(gate.cv.wait_for(lock, std::chrono::seconds(10), [&] { return gate.entered; }));
}

void Release(GatedDownloader::Gate& gate) {
  std::lock_guard<std::mutex> lock(gate.mutex);
  gate.released = true;
  gate.cv.notify_all();
}

CatalogFetchSnapshot WaitDone(const azookey::host::CatalogFetchService& service) {
  for (int i = 0; i < 1000; ++i) {
    auto snapshot = service.Snapshot();
    if (snapshot.state == CatalogFetchSnapshot::State::Done) return snapshot;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ADD_FAILURE() << "the fetch did not finish";
  return service.Snapshot();
}

struct ServiceFixture {
  fs::path dir = TestDir();
  std::shared_ptr<GatedDownloader::Gate> gate = std::make_shared<GatedDownloader::Gate>();
  std::shared_ptr<std::atomic<bool>> network = std::make_shared<std::atomic<bool>>(true);
  std::unique_ptr<azookey::host::CatalogFetchService> service;

  ServiceFixture() {
    auto matcher = std::make_shared<azookey::host::CatalogModelMatcher>(
        TwoEntryCatalog(), dir / "zenzai", CountingSha256{}.Function());
    service = std::make_unique<azookey::host::CatalogFetchService>(
        matcher, [gate = gate] { return std::make_unique<GatedDownloader>(gate); },
        [network = network] { return network->load(); });
  }
  ~ServiceFixture() {
    service.reset();
    fs::remove_all(dir);
  }
};

}  // namespace

TEST(CatalogFetchServiceTest, RunsOneFetchAtATimeAndReportsProgressAndResult) {
  ServiceFixture f;
  EXPECT_EQ(f.service->Snapshot().state, CatalogFetchSnapshot::State::Idle);
  ASSERT_EQ(f.service->Start(""), CatalogFetchStartResult::Started);
  WaitEntered(*f.gate);

  const auto running = f.service->Snapshot();
  EXPECT_EQ(running.state, CatalogFetchSnapshot::State::Running);
  // An empty id is the default entry.
  EXPECT_EQ(running.model_id, "zenz");
  EXPECT_EQ(running.received_bytes, 1024u);
  // No Content-Length: the entry's sizeBytes is the full size.
  EXPECT_EQ(running.total_bytes, 4096u);
  EXPECT_EQ(f.service->Start("edge"), CatalogFetchStartResult::Busy);

  Release(*f.gate);
  const auto done = WaitDone(*f.service);
  EXPECT_EQ(done.status, ModelFetchStatus::Downloaded) << done.error;
  EXPECT_EQ(done.path, f.dir / "zenzai" / "zenz.gguf");
  EXPECT_EQ(done.received_bytes, done.total_bytes);
  EXPECT_TRUE(done.error.empty());

  azookey::ipc::CatalogFetchResponse response;
  azookey::host::FillCatalogFetchResponse(done, response);
  EXPECT_EQ(response.state, "done");
  EXPECT_EQ(response.status, "downloaded");
  EXPECT_EQ(response.path, Utf8(f.dir / "zenzai" / "zenz.gguf"));
  // The next fetch can start once this one is done.
  EXPECT_EQ(f.service->Start("nope"), CatalogFetchStartResult::UnknownModelId);
}

// The start reports its own fetch, taken under the same lock, and a refused start the fetch
// that refused it.
TEST(CatalogFetchServiceTest, StartReportsTheFetchItStartedOrTheOneThatRefusedIt) {
  ServiceFixture f;
  CatalogFetchSnapshot started;
  ASSERT_EQ(f.service->Start("edge", &started), CatalogFetchStartResult::Started);
  EXPECT_EQ(started.state, CatalogFetchSnapshot::State::Running);
  EXPECT_EQ(started.model_id, "edge");
  WaitEntered(*f.gate);
  CatalogFetchSnapshot refused;
  EXPECT_EQ(f.service->Start("zenz", &refused), CatalogFetchStartResult::Busy);
  EXPECT_EQ(refused.state, CatalogFetchSnapshot::State::Running);
  EXPECT_EQ(refused.model_id, "edge");
  Release(*f.gate);
  EXPECT_EQ(WaitDone(*f.service).status, ModelFetchStatus::Downloaded);
}

// Stopping while the existing file is hashed is a cancellation, not another file's content.
TEST(CatalogFetchServiceTest, StopDuringTheExistingFileCheckIsCancelledNotOccupied) {
  const auto dir = TestDir();
  const auto entry = MakeEntry("zenz", "zenz.gguf", kShaA, ZenzaiPromptFormat::ZenzV3);
  WriteBytes(dir / "zenzai" / "zenz.gguf", "placed");
  auto gate = std::make_shared<GatedDownloader::Gate>();
  GatedDownloader downloader(gate);
  azookey::host::ModelFetchHooks hooks;
  hooks.cancelled = [] { return true; };

  const auto result =
      azookey::host::FetchCatalogModel(entry, dir / "zenzai", downloader, {}, hooks);

  EXPECT_EQ(result.status, ModelFetchStatus::Failed);
  EXPECT_EQ(result.error, "cancelled");
  fs::remove_all(dir);
}

TEST(CatalogFetchServiceTest, OfflineFetchesNothing) {
  ServiceFixture f;
  *f.network = false;
  ASSERT_EQ(f.service->Start("edge"), CatalogFetchStartResult::Started);
  const auto done = WaitDone(*f.service);
  EXPECT_EQ(done.status, ModelFetchStatus::Offline);
  EXPECT_TRUE(done.path.empty());
  EXPECT_FALSE(fs::exists(f.dir / "zenzai" / "edge.gguf"));
}

TEST(CatalogFetchServiceTest, ShutdownStopsARunningFetchAndRefusesNewOnes) {
  ServiceFixture f;
  ASSERT_EQ(f.service->Start("edge"), CatalogFetchStartResult::Started);
  WaitEntered(*f.gate);
  f.service->Shutdown();
  const auto done = f.service->Snapshot();
  EXPECT_EQ(done.state, CatalogFetchSnapshot::State::Done);
  EXPECT_EQ(done.status, ModelFetchStatus::Failed);
  EXPECT_EQ(done.error, "cancelled");
  EXPECT_FALSE(fs::exists(f.dir / "zenzai" / "edge.gguf"));
  EXPECT_EQ(f.service->Start("edge"), CatalogFetchStartResult::Unavailable);
}

TEST(CatalogFetchServiceTest, CatalogEntriesReportEachFileState) {
  const auto dir = TestDir();
  WriteBytes(dir / "zenzai" / "zenz.gguf", std::string(kShaA) + "\n");
  azookey::host::CatalogModelMatcher matcher(TwoEntryCatalog(), dir / "zenzai",
                                             CountingSha256{}.Function());
  const auto entries = azookey::host::CatalogEntries(matcher);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].id, "zenz");
  EXPECT_TRUE(entries[0].is_default);
  EXPECT_EQ(entries[0].state, "verified");
  EXPECT_EQ(entries[0].prompt_format, "zenz-v3");
  EXPECT_TRUE(entries[0].fetchable);
  EXPECT_EQ(entries[0].path, Utf8((dir / "zenzai" / "zenz.gguf").lexically_normal()));
  EXPECT_EQ(entries[1].id, "edge");
  EXPECT_FALSE(entries[1].is_default);
  EXPECT_EQ(entries[1].state, "missing");
  EXPECT_EQ(entries[1].prompt_format, "kanakanji-edge-v1");
  fs::remove_all(dir);
}
