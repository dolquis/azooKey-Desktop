#include <gtest/gtest.h>

#include <Windows.h>

#ifdef GetObject
#undef GetObject
#endif

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "azookey/ipc/Json.h"
#include "runner/ReportWriter.h"
#include "runner/ScreenshotCapture.h"

namespace azookey::compat_test {
namespace {

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    wchar_t root[MAX_PATH]{};
    GetTempPathW(MAX_PATH, root);
    path_ = std::filesystem::path(root) /
            ("azookey-compat-test-" + std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(GetTickCount64()));
    std::filesystem::create_directories(path_);
  }
  ~TemporaryDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

TEST(CompatReportWriterTest, WritesStableSchemaAndRedactsUntrustedReasonText) {
  TemporaryDirectory temp;
  TargetConfig target;
  target.id = "notepad";
  target.display_name = "Notepad";
  target.app_id = "Microsoft.WindowsNotepad_8wekyb3d8bbwe!App";
  target.automation_level = "full";
  std::vector<CaseResult> results{
      {"C-001", ResultStatus::Pass, "expected-commit-observed", 12, {}},
      {"C-004", ResultStatus::Fail, "nihongo 日本語 private candidate", 34,
       temp.path() / "failures" / "C-004_fail"},
      {"C-003", ResultStatus::FailingSkip, "caret-rectangle-unavailable", 56, {}},
  };

  ASSERT_TRUE(WriteReports(temp.path(), target, results));
  const std::string json_text = ReadFile(temp.path() / "report.json");
  const std::string markdown = ReadFile(temp.path() / "report.md");
  const auto parsed = azookey::ipc::json::Parse(json_text);
  ASSERT_TRUE(parsed);
  ASSERT_TRUE(parsed->IsObject());
  EXPECT_EQ(parsed->GetInt("schema_version"), 1);
  const auto* summary = parsed->GetObject("summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(azookey::ipc::json::Value(*summary).GetUInt("pass"), 1u);
  EXPECT_EQ(azookey::ipc::json::Value(*summary).GetUInt("fail"), 1u);
  EXPECT_EQ(azookey::ipc::json::Value(*summary).GetUInt("failing_skip"), 1u);
  EXPECT_EQ(json_text.find("nihongo"), std::string::npos);
  EXPECT_EQ(json_text.find("日本語"), std::string::npos);
  EXPECT_EQ(markdown.find("nihongo"), std::string::npos);
  EXPECT_EQ(markdown.find("日本語"), std::string::npos);
  EXPECT_NE(json_text.find("redacted-detail"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(temp.path() / "logs"));
}

TEST(CompatReportWriterTest, RejectsNonEmptyOutputDirectory) {
  TemporaryDirectory temp;
  std::ofstream(temp.path() / "existing.txt") << "existing";
  EXPECT_FALSE(PrepareOutputDirectory(temp.path()));
}

TEST(ScreenshotCaptureTest, GeometryOverlayContainsNoSourcePixels) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 3;
  constexpr int kStride = kWidth * 4;
  std::array<uint8_t, kStride * kHeight> pixels;
  pixels.fill(0x7f);
  RenderGeometryOverlay(pixels.data(), kWidth, kHeight, kStride, POINT{10, 20},
                        {RECT{11, 21, 13, 23}});

  for (size_t offset = 0; offset < pixels.size(); offset += 4) {
    EXPECT_NE(pixels[offset + 0], 0x7f);
    EXPECT_NE(pixels[offset + 1], 0x7f);
    EXPECT_NE(pixels[offset + 2], 0x7f);
    EXPECT_EQ(pixels[offset + 3], 0xff);
  }
}

}  // namespace
}  // namespace azookey::compat_test
