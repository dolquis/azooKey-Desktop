#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "BenchmarkCommandLine.h"
#include "BenchmarkResult.h"

namespace azookey::bench {
namespace {

std::filesystem::path UniqueTempPath(std::string suffix) {
  static std::atomic<uint64_t> counter{0};
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto process_marker = reinterpret_cast<std::uintptr_t>(&counter);
  return std::filesystem::temp_directory_path() /
         ("azookey-benchmark-result-test-" + std::to_string(process_marker) + "-" +
          std::to_string(ticks) + "-" + std::to_string(counter.fetch_add(1)) + std::move(suffix));
}

BenchmarkResult SampleResult(std::string bench = "azookey_bench") {
  BenchmarkResult result;
  result.bench = std::move(bench);
  result.commit = "0123456789abcdef";
  result.config = "Release";
  result.iterations = 200;
  result.latency = LatencyMetrics{1.0, 2.0, 3.0, 4.0};
  result.max_p95_ms = 5.0;
  result.threshold_passed = true;
  return result;
}

TEST(BenchmarkResultTest, SerializesStableAzookeyBenchSchemaSnapshot) {
  EXPECT_EQ(
      SerializeBenchmarkResult(SampleResult()),
      R"({"baseline":{"commit":null,"minimumChangeMs":0.05,"p95ChangePercent":null,"p99ChangePercent":null,"reason":"baseline not provided","status":"not_provided","warning":false,"warningPercent":10},"bench":"azookey_bench","commit":"0123456789abcdef","config":"Release","iterations":200,"latencyMs":{"max":4,"p50":1,"p95":2,"p99":3},"schemaVersion":1,"threshold":{"maxP95Ms":5,"passed":true}})");
}

TEST(BenchmarkResultTest, SerializesStableZenzaiBenchSchemaSnapshot) {
  auto result = SampleResult("azookey_zenzai_bench");
  result.iterations = 50;
  result.max_p95_ms.reset();
  result.decode_phases =
      DecodePhaseBreakdown{{50, 600, LatencyMetrics{10.0, 11.0, 12.0, 13.0}},
                           {50, 300, 150, LatencyMetrics{20.0, 21.0, 22.0, 23.0}}};
  EXPECT_EQ(
      SerializeBenchmarkResult(result),
      R"({"baseline":{"commit":null,"minimumChangeMs":0.05,"p95ChangePercent":null,"p99ChangePercent":null,"reason":"baseline not provided","status":"not_provided","warning":false,"warningPercent":10},"bench":"azookey_zenzai_bench","commit":"0123456789abcdef","config":"Release","decodePhases":{"beam":{"evaluations":150,"latencyMs":{"max":23,"p50":20,"p95":21,"p99":22},"samples":50,"tokens":300},"prompt":{"latencyMs":{"max":13,"p50":10,"p95":11,"p99":12},"samples":50,"tokens":600}},"iterations":50,"latencyMs":{"max":4,"p50":1,"p95":2,"p99":3},"schemaVersion":1,"threshold":{"maxP95Ms":null,"passed":true}})");
}

TEST(BenchmarkResultTest, ComparesMatchingBaselineAndWarnsAboveTenPercent) {
  const auto path = UniqueTempPath("-baseline.json");
  ASSERT_TRUE(WriteBenchmarkResult(path, SerializeBenchmarkResult(SampleResult()), nullptr));

  const auto comparison =
      CompareBaseline(path, "azookey_bench", "Release", LatencyMetrics{1.0, 2.3, 3.2, 4.0});
  std::filesystem::remove(path);

  EXPECT_EQ(comparison.status, "compared");
  ASSERT_TRUE(comparison.p95_change_percent.has_value());
  ASSERT_TRUE(comparison.p99_change_percent.has_value());
  EXPECT_NEAR(*comparison.p95_change_percent, 15.0, 0.0001);
  EXPECT_NEAR(*comparison.p99_change_percent, 6.6666667, 0.0001);
  EXPECT_TRUE(comparison.warning);
}

TEST(BenchmarkResultTest, IgnoresLargePercentageChangesBelowAbsoluteNoiseFloor) {
  auto baseline = SampleResult();
  baseline.latency = LatencyMetrics{0.001, 0.0016, 0.0045, 0.005};
  const auto path = UniqueTempPath("-small-baseline.json");
  ASSERT_TRUE(WriteBenchmarkResult(path, SerializeBenchmarkResult(baseline), nullptr));

  const auto comparison = CompareBaseline(path, "azookey_bench", "Release",
                                          LatencyMetrics{0.0015, 0.0021, 0.0087, 0.01});
  std::filesystem::remove(path);

  EXPECT_EQ(comparison.status, "compared");
  ASSERT_TRUE(comparison.p95_change_percent.has_value());
  ASSERT_TRUE(comparison.p99_change_percent.has_value());
  EXPECT_GT(*comparison.p95_change_percent, comparison.warning_percent);
  EXPECT_GT(*comparison.p99_change_percent, comparison.warning_percent);
  EXPECT_FALSE(comparison.warning);
  EXPECT_EQ(comparison.reason, "within regression warning threshold or noise floor");
}

TEST(BenchmarkResultTest, MissingOrIncompatibleBaselineIsNotARegression) {
  const auto missing = CompareBaseline("missing-benchmark-baseline.json", "azookey_bench",
                                       "Release", LatencyMetrics{});
  EXPECT_EQ(missing.status, "unavailable");
  EXPECT_FALSE(missing.warning);

  const auto path = UniqueTempPath("-invalid.json");
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "{}";
  }
  const auto incompatible = CompareBaseline(path, "azookey_bench", "Release", LatencyMetrics{});
  std::filesystem::remove(path);
  EXPECT_EQ(incompatible.status, "incompatible");
  EXPECT_FALSE(incompatible.warning);
}

TEST(BenchmarkResultTest, OutputDirectoryIsReportedAsWriteFailure) {
  std::string error;
  EXPECT_FALSE(WriteBenchmarkResult(std::filesystem::temp_directory_path(), "{}", &error));
  EXPECT_NE(error.find("failed to open benchmark JSON output"), std::string::npos);
}

TEST(BenchmarkResultTest, WritesUtf8OutputPath) {
  const auto path = UniqueTempPath("-") / Utf8Path("計測結果.json");
  ASSERT_TRUE(std::filesystem::create_directory(path.parent_path()));
  std::string error;
  EXPECT_TRUE(WriteBenchmarkResult(path, "{}", &error)) << error;
  EXPECT_TRUE(std::filesystem::exists(path));
  std::filesystem::remove_all(path.parent_path());
}

TEST(BenchmarkResultTest, FormatsRegressionWarningWithoutChangingThresholdState) {
  auto result = SampleResult();
  result.baseline.status = "compared";
  result.baseline.commit = "baseline-commit";
  result.baseline.p95_change_percent = 12.5;
  result.baseline.p99_change_percent = 20.0;
  result.baseline.warning = true;

  EXPECT_EQ(RegressionWarning(result),
            "benchmark regression warning: p95=12.5% p99=20% baseline=baseline-commit "
            "threshold=10% minimum_change_ms=0.05");
  EXPECT_TRUE(result.threshold_passed);
}

}  // namespace
}  // namespace azookey::bench
