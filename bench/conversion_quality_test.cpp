#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ConversionQuality.h"

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

}  // namespace
}  // namespace azookey::bench
