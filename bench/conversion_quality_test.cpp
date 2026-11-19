#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ConversionQuality.h"
#include "azookey/ipc/Json.h"

namespace azookey::bench {
namespace {

TEST(ConversionQualityTest, CountsUnicodeScalarsForWagnerFischerDistance) {
  EXPECT_EQ(LevenshteinDistance(DecodeUtf8CodePoints("日本語"), DecodeUtf8CodePoints("日本")), 1u);
  EXPECT_EQ(LevenshteinDistance(DecodeUtf8CodePoints("ab"), DecodeUtf8CodePoints("ba")), 2u);
  EXPECT_EQ(DecodeUtf8CodePoints("😀").size(), 1u);
}

TEST(ConversionQualityTest, CalculatesKnownCerPairsAndEmptyReferenceConvention) {
#if !defined(_WIN32)
  GTEST_SKIP() << "NFKC normalization uses the Windows Normaliz API";
#endif
  const auto replacement = EvaluateCandidateMatch("日本誤", "日本語", {});
  EXPECT_EQ(replacement.edit_distance, 1u);
  EXPECT_EQ(replacement.reference_length, 3u);
  EXPECT_DOUBLE_EQ(replacement.cer, 1.0 / 3.0);

  EXPECT_DOUBLE_EQ(EvaluateCandidateMatch("", "", {}).cer, 0.0);
  EXPECT_DOUBLE_EQ(EvaluateCandidateMatch("追加", "", {}).cer, 1.0);
}

TEST(ConversionQualityTest, SeparatesCanonicalAndAcceptableMatches) {
#if !defined(_WIN32)
  GTEST_SKIP() << "NFKC normalization uses the Windows Normaliz API";
#endif
  const auto canonical = EvaluateCandidateMatch("交渉する", "交渉する", {"交渉します"});
  EXPECT_TRUE(canonical.exact_match);
  EXPECT_FALSE(canonical.acceptable_match);

  const auto variant = EvaluateCandidateMatch("交渉します", "交渉する", {"交渉します"});
  EXPECT_FALSE(variant.exact_match);
  EXPECT_TRUE(variant.acceptable_match);
}

TEST(ConversionQualityTest, ReportsRawAndNfkcBoundaryIndependently) {
#if !defined(_WIN32)
  GTEST_SKIP() << "NFKC normalization uses the Windows Normaliz API";
#endif
  EXPECT_EQ(NormalizeNfkc("仮名"), "仮名");
  EXPECT_EQ(NormalizeNfkc("かな"), "かな");

  const auto result = EvaluateCandidateMatch("ＡＢＣ１２３", "ABC123", {});
  EXPECT_FALSE(result.exact_match);
  EXPECT_TRUE(result.nfkc_exact_match);
  EXPECT_GT(result.cer, 0.0);
  EXPECT_DOUBLE_EQ(result.nfkc_cer, 0.0);
}

TEST(ConversionQualityTest, RejectsInvalidUtf8) {
  EXPECT_THROW(DecodeUtf8CodePoints(std::string("\xc0\x80", 2)), std::invalid_argument);
}

TEST(ConversionQualityTest, CanonicalDatasetHashNormalizesLineEndingsAndFramesRelativePath) {
#if !defined(_WIN32)
  GTEST_SKIP() << "SHA-256 uses Windows CNG";
#else
  static std::atomic<uint64_t> sequence{0};
  const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "-" + std::to_string(sequence.fetch_add(1));
  const auto root =
      std::filesystem::temp_directory_path() / ("azookey-conversion-quality-" + unique);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{root};

  const auto lf_path = root / "lf" / "bench" / "data" / "eval.jsonl";
  const auto crlf_path = root / "crlf" / "bench" / "data" / "eval.jsonl";
  const auto other_path = root / "other" / "bench" / "data" / "other.jsonl";
  for (const auto& path : {lf_path, crlf_path, other_path}) {
    std::filesystem::create_directories(path.parent_path());
  }
  const auto write = [](const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good());
  };
  write(lf_path, "{\"input\":\"a\"}\n{\"input\":\"b\"}\n");
  write(crlf_path, "{\"input\":\"a\"}\r\n{\"input\":\"b\"}\r\n");
  write(other_path, "{\"input\":\"a\"}\n{\"input\":\"b\"}\n");

  EXPECT_EQ(EvaluationDatasetSha256(lf_path), EvaluationDatasetSha256(crlf_path));
  EXPECT_NE(EvaluationDatasetSha256(lf_path), EvaluationDatasetSha256(other_path));
#endif
}

#if defined(_WIN32)
// Helpers for the tests below that need Windows CNG and Normaliz. A temporary directory removed
// when the test ends.
class ScopedDirectory {
 public:
  explicit ScopedDirectory(std::string_view name) {
    static std::atomic<uint64_t> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            (std::string(name) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }
  ~ScopedDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteBytes(const std::filesystem::path& path, std::string_view bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open());
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(output.good());
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
#endif

TEST(ConversionQualityTest, DictionaryBundleHashFramesSortedAzdicNamesAndBytes) {
#if !defined(_WIN32)
  GTEST_SKIP() << "SHA-256 uses Windows CNG";
#else
  ScopedDirectory directory("azookey-dictionary-bundle");
  const auto& root = directory.path();
  EXPECT_THROW(DictionaryBundleSha256(root), std::invalid_argument);
  WriteBytes(root / "a.azdic", "x");
  WriteBytes(root / "notes.txt", "ignored");
  // SHA-256 of "a.azdic\0x".
  EXPECT_EQ(DictionaryBundleSha256(root),
            "f25da710924b3f20a8d2b96ec1dff5167ef78f6b83c20a0eedba3127823ecb74");
  WriteBytes(root / "b.azdic", "y");
  // SHA-256 of "a.azdic\0x" + "b.azdic\0y": ordered by name, not by creation.
  EXPECT_EQ(DictionaryBundleSha256(root),
            "8ccb27bb741b326715f6f65cd954f3837e850973ad2138a672c3e7c7cde1808c");
  WriteBytes(root / "b.azdic", "z");
  EXPECT_NE(DictionaryBundleSha256(root),
            "8ccb27bb741b326715f6f65cd954f3837e850973ad2138a672c3e7c7cde1808c");
  EXPECT_THROW(DictionaryBundleSha256(root / "missing"), std::filesystem::filesystem_error);

  // Byte order puts "B" (0x42) before "a" (0x61); NTFS lists a.azdic first (case-insensitive).
  ScopedDirectory mixed_case("azookey-dictionary-bundle-case");
  WriteBytes(mixed_case.path() / "a.azdic", "x");
  WriteBytes(mixed_case.path() / "B.azdic", "y");
  // SHA-256 of "B.azdic\0y" + "a.azdic\0x".
  EXPECT_EQ(DictionaryBundleSha256(mixed_case.path()),
            "bd6df75ff340130618d281c4e75169e3a195dc4c1bd93f833b088f9db4083056");
#endif
}

TEST(ConversionQualityTest, LatencyPercentilesTakeTheFlooredRank) {
  // 101 values: p95 is the 95th and p99 the 99th (0-based) of the sorted values.
  std::vector<double> values;
  for (int i = 100; i >= 0; --i) values.push_back(i * 0.5);
  EXPECT_DOUBLE_EQ(LatencyPercentile(values, 0.95), 47.5);
  EXPECT_DOUBLE_EQ(LatencyPercentile(values, 0.99), 49.5);
  EXPECT_DOUBLE_EQ(LatencyPercentile({3.0, 1.0, 2.0}, 0.99), 2.0);  // floor(0.99 x 2) = 1
  EXPECT_DOUBLE_EQ(LatencyPercentile({}, 0.99), 0.0);
}

TEST(ConversionQualityTest, KatakanaMeansAKatakanaLetter) {
  EXPECT_TRUE(ContainsKatakana("コーヒーを飲む"));
  EXPECT_TRUE(ContainsKatakana("ヴ"));
  EXPECT_TRUE(ContainsKatakana("ヶ月"));
  EXPECT_FALSE(ContainsKatakana("ー"));
  EXPECT_FALSE(ContainsKatakana("・"));
  EXPECT_FALSE(ContainsKatakana("かな漢字"));
  EXPECT_FALSE(ContainsKatakana("ｶﾀｶﾅ"));  // Halfwidth forms are outside U+30A1..U+30FA.
}

TEST(ConversionQualityTest, ReportsKatakanaSubsetP99AndDictionaryCompatibilityKeys) {
#if !defined(_WIN32)
  GTEST_SKIP() << "SHA-256 uses Windows CNG and NFKC uses the Windows Normaliz API";
#else
  ScopedDirectory directory("azookey-conversion-quality-subset");
  const auto data = directory.path() / "bench" / "data" / "eval.jsonl";
  const auto line = [](std::string_view id, std::string_view input, std::string_view expected) {
    return "{\"id\":\"" + std::string(id) + "\",\"input\":\"" + std::string(input) +
           "\",\"expected\":\"" + std::string(expected) +
           "\",\"category\":[\"general\"],\"provenance\":\"authored\"}\n";
  };
  WriteBytes(data, line("a", "こーひー", "コーヒー") + line("b", "てすと", "テスト") +
                       line("c", "こーひー2", "珈琲"));
  ConversionQualityOptions options;
  options.eval_path = data;
  options.output_path = directory.path() / "result.json";
  options.iterations = 1;
  const auto query = [](const std::string& input, const std::string&) {
    // テスト comes back unconverted, so the katakana subset is one of two.
    const std::string surface = input == "こーひー" ? "コーヒー"
                                : input == "てすと" ? "てすと"
                                                    : "珈琲";
    core::Candidate candidate;
    candidate.surface = surface;
    candidate.reading = input;
    return std::vector<core::Candidate>{candidate};
  };
  std::string error;
  ASSERT_TRUE(RunConversionQualityEvaluation(options, query, &error)) << error;
  const auto report = ipc::json::Parse(ReadText(options.output_path));
  ASSERT_TRUE(report && report->IsObject());
  const auto* config = report->FindObject("config");
  ASSERT_NE(config, nullptr);
  EXPECT_EQ(config->at("dictionary_bundle_sha256").AsString(), "none");
  EXPECT_EQ(config->at("lattice").AsString(), "off");
  const auto* summary = report->FindObject("summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_TRUE(summary->at("latency_p99_ms").IsNumber());
  EXPECT_DOUBLE_EQ(summary->at("top1_accuracy").AsNumber(), 2.0 / 3.0);
  const auto* subsets = report->FindObject("by_subset");
  ASSERT_NE(subsets, nullptr);
  const auto& katakana = subsets->at("katakana").AsObject();
  EXPECT_DOUBLE_EQ(katakana.at("n").AsNumber(), 2.0);
  EXPECT_DOUBLE_EQ(katakana.at("top1_accuracy").AsNumber(), 0.5);

  // A baseline taken with the bundled dictionaries is not comparable with this run.
  const auto baseline = directory.path() / "baseline.json";
  std::string text = ReadText(options.output_path);
  const std::string none = "\"dictionary_bundle_sha256\":\"none\"";
  const auto at = text.find(none);
  ASSERT_NE(at, std::string::npos);
  text.replace(at, none.size(), "\"dictionary_bundle_sha256\":\"" + std::string(64, 'a') + "\"");
  WriteBytes(baseline, text);
  options.baseline_path = baseline;
  EXPECT_FALSE(RunConversionQualityEvaluation(options, query, &error));
  EXPECT_EQ(error, "baseline is incompatible: dictionary_bundle_sha256");
  options.dictionary_bundle_sha256 = std::string(64, 'a');
  options.lattice = "on";
  EXPECT_FALSE(RunConversionQualityEvaluation(options, query, &error));
  EXPECT_EQ(error, "baseline is incompatible: lattice");
#endif
}

TEST(ConversionQualityTest, BaselineWithoutNllCountsAsNllOff) {
#if !defined(_WIN32)
  GTEST_SKIP() << "SHA-256 uses Windows CNG and NFKC uses the Windows Normaliz API";
#else
  ScopedDirectory directory("azookey-conversion-quality-nll");
  const auto data = directory.path() / "bench" / "data" / "eval.jsonl";
  WriteBytes(data,
             "{\"id\":\"a\",\"input\":\"かな\",\"expected\":\"仮名\",\"category\":[\"general\"],"
             "\"provenance\":\"authored\"}\n");
  ConversionQualityOptions options;
  options.eval_path = data;
  options.output_path = directory.path() / "result.json";
  options.iterations = 1;
  const auto query = [](const std::string& input, const std::string&) {
    core::Candidate candidate;
    candidate.surface = "仮名";
    candidate.reading = input;
    return std::vector<core::Candidate>{candidate};
  };
  std::string error;
  ASSERT_TRUE(RunConversionQualityEvaluation(options, query, &error)) << error;
  std::string text = ReadText(options.output_path);
  const auto report = ipc::json::Parse(text);
  ASSERT_TRUE(report && report->IsObject());
  EXPECT_EQ(report->FindObject("config")->at("nll").AsString(), "off");

  // A baseline kept before the nll key existed was taken with NLL off (spec section 14.1).
  const std::string key = "\"nll\":\"off\",";
  const auto at = text.find(key);
  ASSERT_NE(at, std::string::npos);
  text.erase(at, key.size());
  const auto baseline = directory.path() / "baseline.json";
  WriteBytes(baseline, text);
  options.baseline_path = baseline;
  EXPECT_TRUE(RunConversionQualityEvaluation(options, query, &error)) << error;
  options.nll = "on";
  EXPECT_FALSE(RunConversionQualityEvaluation(options, query, &error));
  EXPECT_EQ(error, "baseline is incompatible: nll");
#endif
}

}  // namespace
}  // namespace azookey::bench
