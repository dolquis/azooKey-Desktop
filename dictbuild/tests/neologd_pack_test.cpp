#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "azookey/host/NeologdPack.h"
#include "azookey/ipc/Json.h"
#include "azookey/learning/DictionaryStore.h"

namespace {
using namespace azookey;
namespace j = ipc::json;

std::filesystem::path Case(const char* name) {
  return std::filesystem::path(AZOOKEY_DICT_FIXTURE) / "neologd" / name;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

host::NeologdPackManifest Manifest(const char* name) {
  auto parsed =
      host::ParseNeologdPackManifest(ReadText(Case(name) / "neologd_lexicon.manifest.json"));
  if (!parsed.manifest) throw std::runtime_error("fixture manifest: " + parsed.error);
  return *parsed.manifest;
}

class TempDirectory {
 public:
  TempDirectory() {
    for (int attempt = 0; attempt < 16; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("azookey_neologd_pack_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
               std::to_string(next_id_++));
      if (std::filesystem::create_directory(path_)) return;
    }
    throw std::runtime_error("Could not create neologd pack test directory");
  }
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  std::filesystem::path packs() const { return path_ / "packs"; }

 private:
  std::filesystem::path path_;
  static inline std::atomic<uint64_t> next_id_{0};
};

// Stands in for HttpDownloader: "downloads" by copying a fixture to the destination.
class FakeDownloader final : public host::IPackDownloader {
 public:
  explicit FakeDownloader(std::filesystem::path source,
                          host::HttpDownloadStatus status = host::HttpDownloadStatus::Downloaded)
      : source_(std::move(source)), status_(status) {}
  host::HttpDownloadResult Download(const host::HttpDownloadRequest& request) override {
    requests.push_back(request);
    host::HttpDownloadResult result;
    result.status = status_;
    if (status_ == host::HttpDownloadStatus::Downloaded)
      std::filesystem::copy_file(source_, request.destination,
                                 std::filesystem::copy_options::overwrite_existing);
    if (status_ == host::HttpDownloadStatus::Failed) result.error = "fake failure";
    return result;
  }
  std::vector<host::HttpDownloadRequest> requests;

 private:
  std::filesystem::path source_;
  host::HttpDownloadStatus status_;
};

std::string Edited(const char* name, const std::function<void(j::Object&)>& edit) {
  auto value = j::Parse(ReadText(Case(name) / "neologd_lexicon.manifest.json"));
  if (!value) throw std::runtime_error("fixture manifest is not JSON");
  edit(std::get<j::Object>(value->data));
  return j::Stringify(*value);
}

TEST(NeologdPack, PinnedManifestIsUnpublishedAndNeverDownloads) {
  const auto parsed = host::ParseNeologdPackManifest(host::PinnedNeologdPackManifestJson());
  ASSERT_TRUE(parsed.manifest) << parsed.error;
  EXPECT_FALSE(parsed.manifest->published());
  TempDirectory directory;
  FakeDownloader downloader(Case("valid") / "neologd_lexicon-1.azdic");
  const auto result = host::PrepareNeologdPack(*parsed.manifest, directory.packs(), downloader);
  EXPECT_EQ(result.status, host::NeologdPackStatus::MissingPack);
  EXPECT_TRUE(downloader.requests.empty());
}

TEST(NeologdPack, ParsesPublishedManifestAndRejectsInvalidOnes) {
  const auto manifest = Manifest("valid");
  EXPECT_TRUE(manifest.published());
  EXPECT_EQ(manifest.file_name, "neologd_lexicon-1.azdic");
  EXPECT_EQ(manifest.upstream_revision, "1");
  EXPECT_EQ(manifest.size, std::filesystem::file_size(Case("valid") / manifest.file_name));
  const std::vector<std::pair<const char*, std::function<void(j::Object&)>>> cases = {
      {"fields do not match", [](j::Object& o) { o.erase("url"); }},
      {"fields do not match", [](j::Object& o) { o["extra"] = j::Value(true); }},
      {"unsupported manifest", [](j::Object& o) { o["layer_id"] = j::Value(4); }},
      {"unsupported manifest", [](j::Object& o) { o["format_version"] = j::Value(2); }},
      // JSON integers only, as in neologd_pack.py: an integral float is still rejected.
      {"unsupported manifest",
       [](j::Object& o) { o["layer_id"] = j::Value(j::Number{2.0, "2.0"}); }},
      {"non-negative integer",
       [](j::Object& o) {
         const double size = o["size"].AsNumber();
         o["size"] = j::Value(j::Number{size, std::to_string(static_cast<uint64_t>(size)) + ".0"});
       }},
      {"must be https",
       [](j::Object& o) { o["url"] = j::Value("https://example.invalid/a b.azdic"); }},
      {"must be https", [](j::Object& o) { o["url"] = j::Value("http://example.invalid/p"); }},
      // Only one of url and sha256 set is a published manifest with a missing value.
      {"must be https", [](j::Object& o) { o["url"] = j::Value(""); }},
      {"64 lowercase hex", [](j::Object& o) { o["sha256"] = j::Value(""); }},
      {"64 lowercase hex", [](j::Object& o) { o["sha256"] = j::Value(std::string(64, 'A')); }},
      {"versioned pack", [](j::Object& o) { o["file_name"] = j::Value("neologd_lexicon.azdic"); }},
      {"versioned pack", [](j::Object& o) { o["upstream_revision"] = j::Value("../1"); }},
      {"non-negative integer", [](j::Object& o) { o["size"] = j::Value("10"); }},
      {"attribution is missing", [](j::Object& o) { o["attribution"] = j::Value(j::Object{}); }},
  };
  for (const auto& [message, edit] : cases) {
    const auto parsed = host::ParseNeologdPackManifest(Edited("valid", edit));
    EXPECT_FALSE(parsed.manifest) << message;
    EXPECT_NE(parsed.error.find(message), std::string::npos) << parsed.error;
  }
  const auto unpublished = Edited("valid", [](j::Object& o) {
    o["url"] = j::Value("");
    o["sha256"] = j::Value("");
  });
  EXPECT_NE(host::ParseNeologdPackManifest(unpublished).error.find("must not name a pack"),
            std::string::npos);
}

TEST(NeologdPack, DownloadedPackPassesEveryCheckAndLoadsAsLayerTwo) {
  const auto manifest = Manifest("valid");
  TempDirectory directory;
  FakeDownloader downloader(Case("valid") / manifest.file_name);
  const auto result = host::PrepareNeologdPack(manifest, directory.packs(), downloader);
  ASSERT_EQ(result.status, host::NeologdPackStatus::Ready) << result.error;
  ASSERT_EQ(downloader.requests.size(), 1U);
  const auto& request = downloader.requests[0];
  EXPECT_EQ(request.url, L"https://example.invalid/neologd_lexicon-1.azdic");
  EXPECT_EQ(request.destination, directory.packs() / "neologd_lexicon-1.azdic");
  EXPECT_EQ(request.expected_sha256, manifest.sha256);
  EXPECT_EQ(request.max_bytes, manifest.size);
  EXPECT_EQ(result.path, request.destination);

  learning::DictionaryStore store;
  store.EnableLayer(learning::LayerId::Neologd, true);
  ASSERT_TRUE(store.LoadStatic(learning::LayerId::Neologd, result.path));
  learning::LookupContext context;
  const auto entries = store.Lookup("とうきょう", context);
  ASSERT_FALSE(entries.empty());
  EXPECT_EQ(entries.front().source, learning::LayerId::Neologd);
}

TEST(NeologdPack, FailedChecksKeepTheLayerMissing) {
  const std::vector<std::pair<const char*, const char*>> cases = {
      {"wrong_layer", "pack layer id mismatch"},
      {"corrupt", "invalid pack artifact"},
      {"foreign_attribution", "pack attribution mismatch"},
      {"foreign_builder", "pack builder version mismatch"},
  };
  for (const auto& [name, error] : cases) {
    const auto manifest = Manifest(name);
    TempDirectory directory;
    FakeDownloader downloader(Case(name) / manifest.file_name);
    const auto result = host::PrepareNeologdPack(manifest, directory.packs(), downloader);
    EXPECT_EQ(result.status, host::NeologdPackStatus::Failed) << name;
    EXPECT_EQ(result.error, error) << name;
    EXPECT_TRUE(result.path.empty()) << name;
  }
  auto manifest = Manifest("valid");
  manifest.size += 1;
  TempDirectory directory;
  FakeDownloader downloader(Case("valid") / manifest.file_name);
  const auto result = host::PrepareNeologdPack(manifest, directory.packs(), downloader);
  EXPECT_EQ(result.status, host::NeologdPackStatus::Failed);
  EXPECT_EQ(result.error, "pack size mismatch");

  // The host loads only a Ready pack, so a failure leaves the enabled layer in the
  // missing-pack state while the other layers keep answering.
  learning::DictionaryStore store;
  store.EnableLayer(learning::LayerId::Neologd, true);
  EXPECT_FALSE(store.IsAvailable(learning::LayerId::Neologd));
  EXPECT_TRUE(store.Lookup("とうきょう", {}).empty());
}

TEST(NeologdPack, DownloadFailureAndReuseOfAVerifiedCopy) {
  const auto manifest = Manifest("valid");
  {
    // A file where the directory belongs: nothing is requested.
    TempDirectory directory;
    std::ofstream(directory.packs()) << "not a directory";
    FakeDownloader downloader(Case("valid") / manifest.file_name);
    const auto result = host::PrepareNeologdPack(manifest, directory.packs(), downloader);
    EXPECT_EQ(result.status, host::NeologdPackStatus::Failed);
    EXPECT_EQ(result.error, "packs directory unavailable");
    EXPECT_TRUE(downloader.requests.empty());
  }
  {
    TempDirectory directory;
    FakeDownloader downloader(Case("valid") / manifest.file_name, host::HttpDownloadStatus::Failed);
    const auto result = host::PrepareNeologdPack(manifest, directory.packs(), downloader);
    EXPECT_EQ(result.status, host::NeologdPackStatus::Failed);
    EXPECT_EQ(result.error, "pack download failed");
    EXPECT_FALSE(std::filesystem::exists(directory.packs() / manifest.file_name));
  }
  TempDirectory directory;
  std::filesystem::create_directories(directory.packs());
  std::filesystem::copy_file(Case("valid") / manifest.file_name,
                             directory.packs() / manifest.file_name);
  for (const char* stale : {"neologd_lexicon-0.azdic", "neologd_lexicon-0.azdic.part"})
    std::ofstream(directory.packs() / stale) << "old";
  std::ofstream(directory.packs() / "neologd_lexicon-1.azdic.part") << "resume";
  std::ofstream(directory.packs() / "unrelated.txt") << "keep";
  FakeDownloader downloader(Case("valid") / manifest.file_name,
                            host::HttpDownloadStatus::AlreadyValid);
  const auto result = host::PrepareNeologdPack(manifest, directory.packs(), downloader);
  ASSERT_EQ(result.status, host::NeologdPackStatus::Ready) << result.error;
  EXPECT_FALSE(std::filesystem::exists(directory.packs() / "neologd_lexicon-0.azdic"));
  EXPECT_FALSE(std::filesystem::exists(directory.packs() / "neologd_lexicon-0.azdic.part"));
  EXPECT_TRUE(std::filesystem::exists(directory.packs() / "neologd_lexicon-1.azdic.part"));
  EXPECT_TRUE(std::filesystem::exists(directory.packs() / "unrelated.txt"));
  EXPECT_TRUE(std::filesystem::exists(result.path));
}

#ifdef _WIN32
// The production downloader: a copy whose SHA256 matches is reused without network, and a
// missing one fails against an unreachable loopback port instead of loading anything.
TEST(NeologdPack, HttpPackDownloaderReusesVerifiedCopyWithoutNetwork) {
  auto manifest = Manifest("valid");
  manifest.url = "http://127.0.0.1:1/neologd_lexicon-1.azdic";
  TempDirectory directory;
  host::HttpPackDownloader downloader;
  auto result = host::PrepareNeologdPack(manifest, directory.packs(), downloader);
  EXPECT_EQ(result.status, host::NeologdPackStatus::Failed);
  EXPECT_EQ(result.error, "pack download failed");
  std::filesystem::copy_file(Case("valid") / manifest.file_name,
                             directory.packs() / manifest.file_name);
  result = host::PrepareNeologdPack(manifest, directory.packs(), downloader);
  EXPECT_EQ(result.status, host::NeologdPackStatus::Ready) << result.error;
}
#endif

}  // namespace
