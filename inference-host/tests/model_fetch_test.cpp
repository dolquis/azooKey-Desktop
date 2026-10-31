#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "azookey/core/PlatformPaths.h"
#include "azookey/host/ModelCatalog.h"
#include "azookey/host/ModelFetch.h"
#include "azookey/host/ModelsCli.h"

namespace {

namespace fs = std::filesystem;
using azookey::host::HttpDownloadRequest;
using azookey::host::HttpDownloadResult;
using azookey::host::HttpDownloadStatus;
using azookey::host::ModelFetchStatus;

// Per-test directory: CTest runs each test in its own process, possibly in parallel.
fs::path FetchTestDir() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  auto path = fs::temp_directory_path() /
              (std::string("azookey_model_fetch_") + info->test_suite_name() + "_" + info->name());
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
  AppendLe(out, 3, 4);  // version
  AppendLe(out, 1, 8);  // tensor count
  AppendLe(out, 1, 8);  // key-value count
  const std::string key = "general.architecture";
  AppendLe(out, key.size(), 8);
  out += key;
  AppendLe(out, 8, 4);  // string
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

std::string ReadBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Stands in for HttpDownloader without a network: the SHA-256 verdict is configured, not
// computed. It keeps the downloader's contract that a failure never touches the destination.
class FakeDownloader final : public azookey::host::IPackDownloader {
 public:
  enum class Outcome { Serve, ShaMismatch, ExistingValid };

  explicit FakeDownloader(Outcome outcome, std::string body = ValidGgufBytes())
      : outcome_(outcome), body_(std::move(body)) {}

  HttpDownloadResult Download(const HttpDownloadRequest& request) override {
    requests.push_back(request);
    HttpDownloadResult result;
    if (outcome_ == Outcome::ExistingValid) {
      result.status = HttpDownloadStatus::AlreadyValid;
      return result;
    }
    if (request.network_allowed && !request.network_allowed()) {
      result.error = "network disabled";
      return result;
    }
    network_used = true;
    result.bytes_received = body_.size();
    if (outcome_ == Outcome::ShaMismatch) {
      result.error = "downloaded file SHA256 does not match expected value";
      return result;
    }
    WriteBytes(request.destination, body_);
    result.status = HttpDownloadStatus::Downloaded;
    return result;
  }

  std::vector<HttpDownloadRequest> requests;
  bool network_used{false};

 private:
  Outcome outcome_;
  std::string body_;
};

azookey::host::ModelCatalog DefaultCatalog() {
  const auto parsed =
      azookey::host::ParseModelCatalogJson(azookey::host::DefaultModelCatalogJson());
  EXPECT_TRUE(parsed.ok) << parsed.error.value_or("");
  return parsed.catalog;
}

const azookey::host::ModelCatalogEntry& Entry(const azookey::host::ModelCatalog& catalog,
                                              const std::string& id) {
  for (const auto& entry : catalog.models) {
    if (entry.id == id) return entry;
  }
  ADD_FAILURE() << "no catalog entry " << id;
  return catalog.models.front();
}

}  // namespace

TEST(ModelFetchTest, FetchesThePinnedRevisionAndVerifiesTheGguf) {
  const auto dir = FetchTestDir();
  const auto catalog = DefaultCatalog();
  const auto& edge = Entry(catalog, "kanakanji-edge-33m");
  FakeDownloader downloader(FakeDownloader::Outcome::Serve);

  const auto result = azookey::host::FetchCatalogModel(edge, dir / "zenzai", downloader);

  EXPECT_EQ(result.status, ModelFetchStatus::Downloaded) << result.error;
  EXPECT_EQ(result.path, dir / "zenzai" / "kanakanji-edge-33m-Q8_0.gguf");
  EXPECT_TRUE(result.error.empty());
  ASSERT_EQ(downloader.requests.size(), 1u);
  const auto& request = downloader.requests[0];
  EXPECT_EQ(request.url,
            L"https://huggingface.co/yuuki14202028/kanakanji-edge-33m/resolve/"
            L"da588df2db17e5ab69316986823806ec5f911450/kanakanji-edge-33m-Q8_0.gguf");
  EXPECT_EQ(request.destination, result.path);
  EXPECT_EQ(request.expected_sha256, edge.sha256);
  EXPECT_EQ(request.max_bytes, 35806592u);
  fs::remove_all(dir);
}

TEST(ModelFetchTest, ShaMismatchFailsAndKeepsTheExistingFile) {
  const auto dir = FetchTestDir();
  const auto catalog = DefaultCatalog();
  const auto destination = dir / "zenzai" / "ggml-model-Q5_K_M.gguf";
  WriteBytes(destination, "manually placed");
  FakeDownloader downloader(FakeDownloader::Outcome::ShaMismatch);

  const auto result = azookey::host::FetchCatalogModel(Entry(catalog, "zenz-v3.2-small"),
                                                       dir / "zenzai", downloader);

  EXPECT_EQ(result.status, ModelFetchStatus::Failed);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error, "download_failed");
  EXPECT_NE(result.detail.find("SHA256"), std::string::npos);
  EXPECT_TRUE(result.path.empty());
  EXPECT_EQ(ReadBytes(destination), "manually placed");
  fs::remove_all(dir);
}

TEST(ModelFetchTest, DownloadedFileFailingTheGgufChecksIsRemoved) {
  const auto dir = FetchTestDir();
  const auto catalog = DefaultCatalog();
  FakeDownloader downloader(FakeDownloader::Outcome::Serve, "not a gguf at all");

  const auto result = azookey::host::FetchCatalogModel(Entry(catalog, "zenz-v3.2-small"),
                                                       dir / "zenzai", downloader);

  EXPECT_EQ(result.status, ModelFetchStatus::Failed);
  EXPECT_EQ(result.error, "format:magic_mismatch");
  EXPECT_FALSE(fs::exists(dir / "zenzai" / "ggml-model-Q5_K_M.gguf"));
  fs::remove_all(dir);
}

TEST(ModelFetchTest, AlreadyValidFileIsReusedAndNeverRemoved) {
  const auto dir = FetchTestDir();
  const auto catalog = DefaultCatalog();
  const auto destination = dir / "zenzai" / "ggml-model-Q5_K_M.gguf";
  WriteBytes(destination, ValidGgufBytes());
  FakeDownloader reuse(FakeDownloader::Outcome::ExistingValid);

  const auto reused = azookey::host::FetchCatalogModel(Entry(catalog, "zenz-v3.2-small"),
                                                       dir / "zenzai", reuse, [] { return false; });
  EXPECT_EQ(reused.status, ModelFetchStatus::AlreadyValid) << reused.error;
  EXPECT_EQ(reused.path, destination);
  EXPECT_FALSE(reuse.network_used);

  // The SHA-256 matched, so the file is the pinned one even when it fails the checks.
  WriteBytes(destination, "pinned but not loadable");
  const auto invalid =
      azookey::host::FetchCatalogModel(Entry(catalog, "zenz-v3.2-small"), dir / "zenzai", reuse);
  EXPECT_EQ(invalid.error, "format:magic_mismatch");
  EXPECT_TRUE(fs::exists(destination));
  fs::remove_all(dir);
}

TEST(ModelFetchTest, OfflineFetchesNothing) {
  const auto dir = FetchTestDir();
  const auto catalog = DefaultCatalog();
  FakeDownloader downloader(FakeDownloader::Outcome::Serve);

  const auto result = azookey::host::FetchCatalogModel(
      Entry(catalog, "zenz-v3.2-small"), dir / "zenzai", downloader, [] { return false; });

  EXPECT_EQ(result.status, ModelFetchStatus::Offline);
  EXPECT_TRUE(result.error.empty());
  EXPECT_FALSE(downloader.network_used);
  EXPECT_FALSE(fs::exists(dir / "zenzai" / "ggml-model-Q5_K_M.gguf"));
  fs::remove_all(dir);
}

TEST(ModelFetchTest, EntryWithoutSourceIsNotFetchable) {
  const auto dir = FetchTestDir();
  azookey::host::ModelCatalogEntry entry;
  entry.id = "local";
  entry.file_name = "local.gguf";
  entry.sha256 = std::string(64, 'a');
  FakeDownloader downloader(FakeDownloader::Outcome::Serve);

  const auto result = azookey::host::FetchCatalogModel(entry, dir / "zenzai", downloader);

  EXPECT_EQ(result.status, ModelFetchStatus::NotFetchable);
  EXPECT_TRUE(downloader.requests.empty());
  fs::remove_all(dir);
}

TEST(ModelsCliTest, ParsesCatalogAndFetchArguments) {
  std::string error;
  const auto catalog = azookey::host::ParseModelsCliArgs({"catalog"}, &error);
  ASSERT_TRUE(catalog) << error;
  EXPECT_EQ(catalog->command, azookey::host::ModelsCliCommand::Catalog);

  const auto fetch =
      azookey::host::ParseModelsCliArgs({"fetch", "--id", "kanakanji-edge-33m"}, &error);
  ASSERT_TRUE(fetch) << error;
  EXPECT_EQ(fetch->command, azookey::host::ModelsCliCommand::Fetch);
  EXPECT_EQ(fetch->fetch_model_id, "kanakanji-edge-33m");

  const auto fetch_default = azookey::host::ParseModelsCliArgs({"fetch"}, &error);
  ASSERT_TRUE(fetch_default) << error;
  EXPECT_TRUE(fetch_default->fetch_model_id.empty());

  EXPECT_FALSE(azookey::host::ParseModelsCliArgs({"fetch", "--id"}, &error));
  EXPECT_FALSE(azookey::host::ParseModelsCliArgs({"catalog", "--id", "x"}, &error));
  EXPECT_FALSE(azookey::host::ParseModelsCliArgs({"fetch", "--url", "x"}, &error));
}

TEST(ModelsCliTest, CatalogListsTheEmbeddedEntriesAndWhetherTheyArePresent) {
  const auto dir = FetchTestDir();
  WriteBytes(dir / "models" / "zenzai" / "kanakanji-edge-33m-Q8_0.gguf", ValidGgufBytes());
  azookey::host::ModelsCliOptions options;
  options.command = azookey::host::ModelsCliCommand::Catalog;
  azookey::host::ModelsCliRunOptions run;
  run.models_dir = dir / "models";

  const auto result = azookey::host::RunModelsCli(options, run);

  EXPECT_EQ(result.exit_code, 0) << result.error;
  ASSERT_EQ(result.output_lines.size(), 2u);
  EXPECT_EQ(
      result.output_lines[0].rfind("zenz-v3.2-small\tdefault\tzenz-v3\t73871936\tmissing\t", 0), 0u)
      << result.output_lines[0];
  EXPECT_EQ(result.output_lines[1].rfind(
                "kanakanji-edge-33m\t-\tkanakanji-edge-v1\t35806592\tpresent\t", 0),
            0u)
      << result.output_lines[1];
  fs::remove_all(dir);
}

TEST(ModelsCliTest, FetchDefaultsToTheCatalogDefaultAndReportsThePath) {
  const auto dir = FetchTestDir();
  FakeDownloader downloader(FakeDownloader::Outcome::Serve);
  azookey::host::ModelsCliOptions options;
  options.command = azookey::host::ModelsCliCommand::Fetch;
  azookey::host::ModelsCliRunOptions run;
  run.models_dir = dir / "models";
  run.settings_path = dir / "config" / "settings.json";  // missing: the defaults
  run.downloader = &downloader;

  const auto result = azookey::host::RunModelsCli(options, run);

  EXPECT_EQ(result.exit_code, 0) << result.error;
  ASSERT_EQ(result.output_lines.size(), 1u);
  const auto expected_path =
      azookey::core::PathToUtf8(dir / "models" / "zenzai" / "ggml-model-Q5_K_M.gguf");
  EXPECT_EQ(result.output_lines[0], "status=downloaded id=zenz-v3.2-small bytes=" +
                                        std::to_string(ValidGgufBytes().size()) +
                                        " path=" + expected_path);
  EXPECT_TRUE(downloader.network_used);
  fs::remove_all(dir);
}

TEST(ModelsCliTest, FetchIsRefusedOfflineAndWhenSettingsCannotBeRead) {
  const auto dir = FetchTestDir();
  const auto settings = dir / "config" / "settings.json";
  for (const std::string& content :
       {std::string(R"({"privacy": {"mode": "offline"}})"), std::string("{not json")}) {
    WriteBytes(settings, content);
    FakeDownloader downloader(FakeDownloader::Outcome::Serve);
    azookey::host::ModelsCliOptions options;
    options.command = azookey::host::ModelsCliCommand::Fetch;
    options.fetch_model_id = "kanakanji-edge-33m";
    azookey::host::ModelsCliRunOptions run;
    run.models_dir = dir / "models";
    run.settings_path = settings;
    run.downloader = &downloader;

    const auto result = azookey::host::RunModelsCli(options, run);

    EXPECT_EQ(result.exit_code, 1) << content;
    ASSERT_EQ(result.output_lines.size(), 1u);
    EXPECT_EQ(result.output_lines[0], "status=offline id=kanakanji-edge-33m bytes=0");
    EXPECT_NE(result.error.find("offline"), std::string::npos);
    EXPECT_FALSE(downloader.network_used) << content;
  }

  // Another mode allows it.
  WriteBytes(settings, R"({"privacy": {"mode": "private"}})");
  FakeDownloader downloader(FakeDownloader::Outcome::Serve);
  azookey::host::ModelsCliOptions options;
  options.command = azookey::host::ModelsCliCommand::Fetch;
  azookey::host::ModelsCliRunOptions run;
  run.models_dir = dir / "models";
  run.settings_path = settings;
  run.downloader = &downloader;
  EXPECT_EQ(azookey::host::RunModelsCli(options, run).exit_code, 0);
  EXPECT_TRUE(downloader.network_used);
  fs::remove_all(dir);
}

TEST(ModelsCliTest, FetchRejectsAnIdOutsideTheCatalog) {
  const auto dir = FetchTestDir();
  FakeDownloader downloader(FakeDownloader::Outcome::Serve);
  azookey::host::ModelsCliOptions options;
  options.command = azookey::host::ModelsCliCommand::Fetch;
  options.fetch_model_id = "missing";
  azookey::host::ModelsCliRunOptions run;
  run.models_dir = dir / "models";
  run.downloader = &downloader;

  const auto result = azookey::host::RunModelsCli(options, run);

  EXPECT_EQ(result.exit_code, 2);
  EXPECT_NE(result.error.find("missing"), std::string::npos);
  EXPECT_TRUE(downloader.requests.empty());
  fs::remove_all(dir);
}
