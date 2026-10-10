#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "azookey/core/PlatformPaths.h"
#include "azookey/host/ModelBenchmark.h"
#include "azookey/host/ModelBenchmarkWorker.h"
#include "azookey/host/ModelScanner.h"
#include "azookey/host/ModelsCli.h"
#include "azookey/ipc/Json.h"
#include "azookey/ipc/Payloads.h"

namespace {

namespace fs = std::filesystem;
using azookey::host::LocalModelFormat;

// Per-test directory: CTest runs each test in its own process, possibly in parallel.
fs::path TestDir() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  auto path = fs::temp_directory_path() / (std::string("azookey_model_scanner_") +
                                           info->test_suite_name() + "_" + info->name());
  fs::remove_all(path);
  fs::create_directories(path);
  return path;
}

void AppendU32(std::string& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

void AppendU64(std::string& out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

void AppendString(std::string& out, const std::string& value) {
  AppendU64(out, value.size());
  out += value;
}

struct GgufFixture {
  uint32_t version{3};
  uint64_t tensor_count{1};
  bool architecture{true};
  bool file_type{true};
  bool long_array_first{false};
};

// A GGUF header with the key-value pairs ListModels reads; no tensor data.
std::string GgufHeader(const GgufFixture& fixture) {
  std::string out = "GGUF";
  AppendU32(out, fixture.version);
  AppendU64(out, fixture.tensor_count);
  const uint64_t kv_count = 1 + (fixture.architecture ? 1 : 0) + (fixture.file_type ? 1 : 0) +
                            (fixture.long_array_first ? 1 : 0);
  AppendU64(out, kv_count);
  if (fixture.long_array_first) {
    // An array larger than the 4 KiB window: everything after it is unread.
    AppendString(out, "tokenizer.ggml.scores");
    AppendU32(out, 9);  // array
    AppendU32(out, 6);  // f32
    AppendU64(out, 4000);
    out.append(4000 * 4, '\0');
  }
  AppendString(out, "general.name");
  AppendU32(out, 8);  // string
  AppendString(out, "fixture");
  if (fixture.architecture) {
    AppendString(out, "general.architecture");
    AppendU32(out, 8);
    AppendString(out, "gpt2");
  }
  if (fixture.file_type) {
    AppendString(out, "general.file_type");
    AppendU32(out, 4);  // u32
    AppendU32(out, 15);
  }
  return out;
}

void WriteFile(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void WriteGenAiModel(const fs::path& dir, const std::string& config) {
  WriteFile(dir / "genai_config.json", config);
  WriteFile(dir / "model.onnx", "onnx");
  WriteFile(dir / "tokenizer.json", "{}");
}

constexpr char kGenAiConfig[] = R"({"model":{"type":"gpt2","decoder":{"filename":"model.onnx"}}})";

const azookey::host::LocalModelEntry* Find(const std::vector<azookey::host::LocalModelEntry>& all,
                                           const std::string& file_name) {
  for (const auto& entry : all) {
    if (entry.file_name == file_name) return &entry;
  }
  return nullptr;
}

}  // namespace

TEST(ModelScannerTest, MissingAndEmptyDirectoriesYieldNoModels) {
  const auto dir = TestDir();
  EXPECT_TRUE(azookey::host::ScanModelDirectory(dir).empty());
  EXPECT_TRUE(azookey::host::ScanModelDirectory(dir / "missing").empty());
  fs::remove_all(dir);
}

TEST(ModelScannerTest, ReadsGgufHeaderMetadata) {
  const auto dir = TestDir();
  WriteFile(dir / "zenzai.gguf", GgufHeader({}));
  WriteFile(dir / "UPPER.GGUF", GgufHeader({}));
  WriteFile(dir / "notes.txt", "ignored");
  const auto models = azookey::host::ScanModelDirectory(dir);
  ASSERT_EQ(models.size(), 2u);
  const auto* entry = Find(models, "zenzai.gguf");
  ASSERT_NE(entry, nullptr);
  EXPECT_TRUE(entry->valid) << entry->invalid_reason;
  EXPECT_EQ(entry->format, LocalModelFormat::Gguf);
  EXPECT_EQ(entry->model_family, "gpt2");
  EXPECT_EQ(entry->quantization, "Q4_K_M");
  EXPECT_EQ(entry->size_bytes, GgufHeader({}).size());
  EXPECT_NE(Find(models, "UPPER.GGUF"), nullptr);
  fs::remove_all(dir);
}

TEST(ModelScannerTest, BrokenGgufFilesStayListedAsInvalid) {
  const auto dir = TestDir();
  WriteFile(dir / "magic.gguf", "NOPE" + GgufHeader({}).substr(4));
  GgufFixture v1;
  v1.version = 1;
  WriteFile(dir / "v1.gguf", GgufHeader(v1));
  WriteFile(dir / "short.gguf", "GGUF\x03");
  WriteFile(dir / "empty.gguf", "");
  GgufFixture no_tensors;
  no_tensors.tensor_count = 0;
  WriteFile(dir / "no_tensors.gguf", GgufHeader(no_tensors));
  GgufFixture no_architecture;
  no_architecture.architecture = false;
  WriteFile(dir / "no_arch.gguf", GgufHeader(no_architecture));
  const auto models = azookey::host::ScanModelDirectory(dir);
  ASSERT_EQ(models.size(), 6u);
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"magic.gguf", "magic_mismatch"},      {"v1.gguf", "unsupported_version"},
      {"short.gguf", "truncated_header"},    {"empty.gguf", "truncated_header"},
      {"no_tensors.gguf", "invalid_header"}, {"no_arch.gguf", "missing_metadata"},
  };
  for (const auto& [name, reason] : expected) {
    const auto* entry = Find(models, name);
    ASSERT_NE(entry, nullptr) << name;
    EXPECT_FALSE(entry->valid) << name;
    EXPECT_EQ(entry->invalid_reason, reason) << name;
  }
  fs::remove_all(dir);
}

TEST(ModelScannerTest, MetadataPastTheHeaderWindowIsNotADefect) {
  const auto dir = TestDir();
  GgufFixture fixture;
  fixture.long_array_first = true;
  WriteFile(dir / "big.gguf", GgufHeader(fixture));
  const auto models = azookey::host::ScanModelDirectory(dir);
  ASSERT_EQ(models.size(), 1u);
  EXPECT_TRUE(models[0].valid) << models[0].invalid_reason;
  EXPECT_TRUE(models[0].model_family.empty());
  fs::remove_all(dir);
}

TEST(ModelScannerTest, ScansOneSubdirectoryLevelAndNonAsciiPaths) {
  const auto dir = TestDir();
  const auto japanese = dir / azookey::core::Utf8Path("モデル");
  WriteFile(japanese / azookey::core::Utf8Path("日本語.gguf"), GgufHeader({}));
  WriteFile(japanese / "deeper" / "hidden.gguf", GgufHeader({}));
  const auto models = azookey::host::ScanModelDirectory(dir);
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models[0].file_name, "日本語.gguf");
  EXPECT_TRUE(models[0].valid);
  const auto listed = azookey::host::ToListedModel(models[0], "not_loaded", {});
  EXPECT_EQ(azookey::core::Utf8Path(listed.path), models[0].path);
  EXPECT_NE(listed.path.find("モデル"), std::string::npos);
  fs::remove_all(dir);
}

TEST(ModelScannerTest, ValidatesOnnxGenAiDirectories) {
  const auto dir = TestDir();
  WriteGenAiModel(dir / "good", kGenAiConfig);
  WriteGenAiModel(dir / "bad_json", "{");
  WriteGenAiModel(dir / "missing_onnx",
                  R"({"model":{"type":"gpt2","decoder":{"filename":"other.onnx"}}})");
  WriteGenAiModel(dir / "escape",
                  R"({"model":{"type":"gpt2","decoder":{"filename":"../good/model.onnx"}}})");
  WriteGenAiModel(
      dir / "pipeline",
      R"({"model":{"type":"phi","decoder":{"pipeline":[)"
      R"({"embeddings":{"filename":"model.onnx"}},{"decode":{"filename":"x.onnx"}}]}}})");
  WriteGenAiModel(dir / "no_tokenizer", kGenAiConfig);
  fs::remove(dir / "no_tokenizer" / "tokenizer.json");
  const auto models = azookey::host::ScanModelDirectory(dir);
  ASSERT_EQ(models.size(), 6u);
  const auto* good = Find(models, "good");
  ASSERT_NE(good, nullptr);
  EXPECT_EQ(good->format, LocalModelFormat::OnnxGenAi);
  EXPECT_TRUE(good->valid) << good->invalid_reason;
  EXPECT_EQ(good->model_family, "gpt2");
  EXPECT_GT(good->size_bytes, 0u);
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"bad_json", "invalid_genai_config"},  {"missing_onnx", "missing_model_file"},
      {"escape", "missing_model_file"},      {"pipeline", "missing_model_file"},
      {"no_tokenizer", "missing_tokenizer"},
  };
  for (const auto& [name, reason] : expected) {
    const auto* entry = Find(models, name);
    ASSERT_NE(entry, nullptr) << name;
    EXPECT_FALSE(entry->valid) << name;
    EXPECT_EQ(entry->invalid_reason, reason) << name;
  }
  fs::remove_all(dir);
}

TEST(ModelScannerTest, ListingDirectoryMustStayInsideTheModelsRoot) {
  const auto dir = TestDir();
  const auto root = dir / "models";
  fs::create_directories(root / "zenzai");
  using azookey::host::ResolveModelListingDirectory;
  EXPECT_EQ(ResolveModelListingDirectory("", root), fs::weakly_canonical(root));
  EXPECT_EQ(ResolveModelListingDirectory(azookey::core::PathToUtf8(root / "zenzai"), root),
            fs::weakly_canonical(root / "zenzai"));
  EXPECT_FALSE(ResolveModelListingDirectory(azookey::core::PathToUtf8(dir), root));
  EXPECT_FALSE(
      ResolveModelListingDirectory(azookey::core::PathToUtf8(root / ".." / "elsewhere"), root));
  EXPECT_FALSE(ResolveModelListingDirectory("relative/models", root));
  EXPECT_FALSE(ResolveModelListingDirectory("", fs::path()));
  fs::remove_all(dir);
}

TEST(ModelScannerTest, ListedModelCarriesTheInvalidReason) {
  azookey::host::LocalModelEntry entry;
  entry.path = azookey::core::Utf8Path("C:/models/a.gguf");
  entry.file_name = "a.gguf";
  entry.invalid_reason = "magic_mismatch";
  const auto listed = azookey::host::ToListedModel(entry, "not_loaded", "load_failed");
  EXPECT_EQ(listed.format, "gguf");
  EXPECT_FALSE(listed.valid);
  EXPECT_EQ(listed.last_error, "magic_mismatch");
  entry.valid = true;
  EXPECT_EQ(azookey::host::ToListedModel(entry, "failed", "load_failed").last_error, "load_failed");
}

TEST(ModelBenchmarkTest, RejectsInvalidRequestsWithoutLoading) {
  const auto dir = TestDir();
  WriteFile(dir / "model.gguf", GgufHeader({}));
  azookey::ipc::BenchmarkModelRequest request;
  request.path = azookey::core::PathToUtf8(dir / "model.gguf");
  const auto run = [](azookey::ipc::BenchmarkModelRequest r) {
    return azookey::host::RunModelBenchmark(r, azookey::host::ModelBenchmarkOptions{});
  };

  auto bad_backend = request;
  bad_backend.backend = "winml";
  EXPECT_EQ(run(bad_backend).error, "unsupported_backend");
  auto zero = request;
  zero.iterations = 0;
  EXPECT_EQ(run(zero).error, "invalid_request");
  auto too_many = request;
  too_many.iterations = azookey::host::kMaxBenchmarkIterations + 1;
  EXPECT_EQ(run(too_many).error, "invalid_request");
  auto empty_case = request;
  empty_case.cases = {""};
  EXPECT_EQ(run(empty_case).error, "invalid_request");
  auto broken = request;
  WriteFile(dir / "broken.gguf", "NOPE");
  broken.path = azookey::core::PathToUtf8(dir / "broken.gguf");
  const auto invalid = run(broken);
  EXPECT_EQ(invalid.status, "error");
  EXPECT_EQ(invalid.error, "invalid_model");
  EXPECT_EQ(invalid.iterations_completed, 0u);
  auto onnx = request;
  WriteGenAiModel(dir / "onnx", kGenAiConfig);
  onnx.path = azookey::core::PathToUtf8(dir / "onnx");
  EXPECT_EQ(run(onnx).error, "invalid_model");
  fs::remove_all(dir);
}

#if !AZOOKEY_WITH_LLAMA_CPP
// The header-only fixture loads only in the probe-only (no llama.cpp) build.
TEST(ModelBenchmarkTest, ReportsPercentilesAndHonorsTheBudget) {
  const auto dir = TestDir();
  WriteFile(dir / "model.gguf", GgufHeader({}));
  azookey::ipc::BenchmarkModelRequest request;
  request.path = azookey::core::PathToUtf8(dir / "model.gguf");
  request.iterations = 5;
  request.warmup = 1;
  azookey::host::ModelBenchmarkOptions options;
  options.mock_zenzai_candidates_for_tests = true;
  options.rss_mb = [] { return 42.0; };
  const auto done = azookey::host::RunModelBenchmarkInline(request, options);
  EXPECT_EQ(done.status, "success") << done.error.value_or("");
  EXPECT_FALSE(done.error.has_value());
  EXPECT_EQ(done.backend, "cpu");
  EXPECT_EQ(done.iterations_completed, 5u);
  EXPECT_LE(done.p50_ms, done.p95_ms);
  EXPECT_LE(done.p95_ms, done.p99_ms);
  EXPECT_GE(done.load_ms, 0.0);
  EXPECT_DOUBLE_EQ(done.rss_mb, 42.0);
  EXPECT_FALSE(done.vram_mb.has_value());

  options.budget = std::chrono::milliseconds{0};
  const auto timed_out = azookey::host::RunModelBenchmarkInline(request, options);
  EXPECT_EQ(timed_out.status, "timeout");
  EXPECT_EQ(timed_out.iterations_completed, 0u);
  fs::remove_all(dir);
}

TEST(ModelBenchmarkTest, InlineSnapshotsRetainMeasuredRssBeforeUnloadOrInterruption) {
  const auto dir = TestDir();
  WriteFile(dir / "model.gguf", GgufHeader({}));
  azookey::ipc::BenchmarkModelRequest request;
  request.path = azookey::core::PathToUtf8(dir / "model.gguf");
  request.iterations = 3;
  request.warmup = 0;
  azookey::host::ModelBenchmarkOptions options;
  options.mock_zenzai_candidates_for_tests = true;
  double rss = 42.0;
  options.rss_mb = [&] { return rss; };
  azookey::host::BenchmarkWorkerTestHooks hooks;
  hooks.before_query = [&](const auto&, uint64_t i) { rss = 100.0 + static_cast<double>(i); };
  hooks.before_unload = [&](const auto&) { rss = 999.0; };
  std::vector<azookey::ipc::BenchmarkModelResponse> snapshots;
  const auto publish = [&](const auto& response) { snapshots.push_back(response); };

  const auto done = azookey::host::RunModelBenchmarkInline(request, options, publish, hooks);
  ASSERT_EQ(snapshots.size(), 5u);
  EXPECT_DOUBLE_EQ(snapshots.front().rss_mb, 42.0);
  EXPECT_EQ(snapshots.front().iterations_completed, 0u);
  for (size_t i = 1; i <= request.iterations; ++i) {
    EXPECT_EQ(snapshots[i].iterations_completed, i);
    EXPECT_DOUBLE_EQ(snapshots[i].rss_mb, 99.0 + static_cast<double>(i));
  }
  EXPECT_EQ(done.status, "success");
  EXPECT_EQ(snapshots.back().status, "success");
  EXPECT_DOUBLE_EQ(done.rss_mb, 102.0);
  EXPECT_DOUBLE_EQ(snapshots.back().rss_mb, 102.0);
  EXPECT_DOUBLE_EQ(rss, 999.0);

  snapshots.clear();
  rss = 42.0;
  hooks.before_query = [&](const auto&, uint64_t i) {
    rss = 100.0 + static_cast<double>(i);
    if (i == 2) throw std::runtime_error("interrupted query");
  };
  EXPECT_THROW(azookey::host::RunModelBenchmarkInline(request, options, publish, hooks),
               std::runtime_error);
  ASSERT_EQ(snapshots.size(), 3u);
  EXPECT_EQ(snapshots.back().iterations_completed, 2u);
  EXPECT_DOUBLE_EQ(snapshots.back().rss_mb, 101.0);
  EXPECT_DOUBLE_EQ(rss, 102.0);
  fs::remove_all(dir);
}
#endif

TEST(ModelsCliTest, ParsesListAndBenchArguments) {
  std::string error;
  const auto list =
      azookey::host::ParseModelsCliArgs({"list", "--dir", "x", "--sha256", "--json"}, &error);
  ASSERT_TRUE(list) << error;
  EXPECT_EQ(list->command, azookey::host::ModelsCliCommand::List);
  EXPECT_TRUE(list->json);
  EXPECT_TRUE(list->list.compute_sha256);
  EXPECT_EQ(list->list.directory, "x");

  const auto bench = azookey::host::ParseModelsCliArgs(
      {"bench", "--path", "m.gguf", "--iterations", "7", "--warmup", "2", "--case", "かな"},
      &error);
  ASSERT_TRUE(bench) << error;
  EXPECT_EQ(bench->bench.path, "m.gguf");
  EXPECT_EQ(bench->bench.iterations, 7u);
  EXPECT_EQ(bench->bench.warmup, 2u);
  EXPECT_EQ(bench->bench.cases, (std::vector<std::string>{"かな"}));

  EXPECT_FALSE(azookey::host::ParseModelsCliArgs({}, &error));
  EXPECT_FALSE(azookey::host::ParseModelsCliArgs({"bench"}, &error));
  EXPECT_FALSE(
      azookey::host::ParseModelsCliArgs({"bench", "--path", "m", "--iterations", "-1"}, &error));
  EXPECT_FALSE(azookey::host::ParseModelsCliArgs({"list", "--path", "m"}, &error));
}

TEST(ModelsCliTest, ListJsonIsTheListModelsPayload) {
  const auto dir = TestDir();
  WriteFile(dir / "zenzai.gguf", GgufHeader({}));
  azookey::host::ModelsCliOptions options;
  options.json = true;
  azookey::host::ModelsCliRunOptions run;
  run.models_dir = dir;
  const auto result = azookey::host::RunModelsCli(options, run);
  EXPECT_EQ(result.exit_code, 0) << result.error;
  ASSERT_EQ(result.output_lines.size(), 1u);
  const auto parsed = azookey::ipc::ParseListModelsResponse(result.output_lines[0]);
  ASSERT_TRUE(parsed);
  ASSERT_EQ(parsed->models.size(), 1u);
  EXPECT_EQ(parsed->models[0].file_name, "zenzai.gguf");
  EXPECT_EQ(parsed->models[0].last_load_status, "not_loaded");

  options.list.directory = azookey::core::PathToUtf8(dir.parent_path());
  const auto outside = azookey::host::RunModelsCli(options, run);
  EXPECT_EQ(outside.exit_code, 2);
  fs::remove_all(dir);
}

TEST(ModelScannerTest, DoesNotFollowLinksOutOfTheModelsDirectory) {
  const auto dir = TestDir();
  const auto elsewhere = dir / "elsewhere";
  WriteFile(elsewhere / "outside.gguf", GgufHeader({}));
  WriteFile(dir / "models" / "inside.gguf", GgufHeader({}));
  std::error_code ec;
  fs::create_directory_symlink(elsewhere, dir / "models" / "linked", ec);
  if (ec) {
    fs::remove_all(dir);
    GTEST_SKIP() << "creating a directory symlink needs Developer Mode or privilege here";
  }
  fs::create_symlink(elsewhere / "outside.gguf", dir / "models" / "file-link.gguf", ec);
  const auto models = azookey::host::ScanModelDirectory(dir / "models");
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models[0].file_name, "inside.gguf");
  fs::remove_all(dir);
}

#ifdef _WIN32
TEST(ModelScannerTest, AnUnconvertibleNameDoesNotFailTheListing) {
  const auto dir = TestDir();
  WriteFile(dir / "good.gguf", GgufHeader({}));
  // A lone surrogate is a legal NTFS name but has no UTF-8 spelling.
  const fs::path bad = dir / std::wstring(L"bad\xD800.gguf");
  {
    std::ofstream out(bad, std::ios::binary);
    if (!out) {
      fs::remove_all(dir);
      GTEST_SKIP() << "the file system rejected the lone surrogate name";
    }
    const auto header = GgufHeader({});
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
  }
  const auto models = azookey::host::ScanModelDirectory(dir);
  ASSERT_FALSE(models.empty());
  EXPECT_NE(Find(models, "good.gguf"), nullptr);
  for (const auto& entry : models) {
    EXPECT_NO_THROW((void)azookey::host::ToListedModel(entry, "not_loaded", {}));
  }
  fs::remove_all(dir);
}

TEST(ModelScannerTest, ComputesSha256ForValidGgufFiles) {
  const auto dir = TestDir();
  WriteFile(dir / "zenzai.gguf", GgufHeader({}));
  WriteFile(dir / "broken.gguf", "NOPE");
  azookey::host::ModelScanOptions options;
  options.compute_sha256 = true;
  const auto models = azookey::host::ScanModelDirectory(dir, options);
  const auto* valid = Find(models, "zenzai.gguf");
  ASSERT_NE(valid, nullptr);
  EXPECT_EQ(valid->sha256.size(), 64u);
  EXPECT_EQ(valid->sha256.find_first_not_of("0123456789abcdef"), std::string::npos);
  const auto* broken = Find(models, "broken.gguf");
  ASSERT_NE(broken, nullptr);
  EXPECT_TRUE(broken->sha256.empty());  // Only valid R1 files are hashed.
  EXPECT_TRUE(azookey::host::ScanModelDirectory(dir).front().sha256.empty());
  fs::remove_all(dir);
}
#endif

TEST(ModelBenchmarkTest, OnlyOneBenchmarkHoldsTheSlot) {
  auto first = azookey::host::TryAcquireBenchmarkSlot();
  ASSERT_TRUE(first.owns_lock());
  EXPECT_FALSE(azookey::host::TryAcquireBenchmarkSlot().owns_lock());
  first.unlock();
  EXPECT_TRUE(azookey::host::TryAcquireBenchmarkSlot().owns_lock());
}
