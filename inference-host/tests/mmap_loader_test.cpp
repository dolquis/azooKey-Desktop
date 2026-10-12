#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "azookey/host/MmapModelLoader.h"

namespace {

using azookey::host::LoadWithMmapFallback;
using azookey::host::MmapFallbackReason;
using azookey::host::MmapLoadReport;
using azookey::host::MmapModelLoader;

std::filesystem::path TempFile(const std::string& name, const std::string& contents) {
  const auto path = std::filesystem::temp_directory_path() / ("azookey_mmap_" + name);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return path;
}

struct Attempt {
  bool ok{false};
  bool use_mmap{false};
};

TEST(MmapModelLoaderTest, MapsTheWholeFile) {
  static constexpr char kBytes[] = "GGUF\x03\x00\x00\x00model bytes";
  const std::string contents(kBytes, sizeof(kBytes) - 1);
  const auto path = TempFile("whole.gguf", contents);
  {
    MmapModelLoader loader;
    ASSERT_TRUE(loader.Load(path)) << loader.error();
    ASSERT_EQ(loader.Size(), contents.size());
    EXPECT_EQ(std::memcmp(loader.Data(), contents.data(), contents.size()), 0);
    EXPECT_TRUE(loader.error().empty());
  }
  std::filesystem::remove(path);
}

TEST(MmapModelLoaderTest, MissingFileFails) {
  MmapModelLoader loader;
  EXPECT_FALSE(loader.Load(std::filesystem::temp_directory_path() / "azookey_mmap_missing.gguf"));
  EXPECT_EQ(loader.Data(), nullptr);
  EXPECT_EQ(loader.Size(), 0u);
  EXPECT_FALSE(loader.error().empty());
}

TEST(MmapModelLoaderTest, EmptyFileCannotBeMapped) {
  const auto path = TempFile("empty.gguf", "");
  {
    MmapModelLoader loader;
    EXPECT_FALSE(loader.Load(path));
    EXPECT_EQ(loader.Data(), nullptr);
    EXPECT_FALSE(loader.error().empty());
  }
  std::filesystem::remove(path);
}

TEST(MmapModelLoaderTest, UnloadReleasesTheFile) {
  const auto path = TempFile("unload.gguf", "GGUF payload");
  MmapModelLoader loader;
  ASSERT_TRUE(loader.Load(path));
  loader.Unload();
  EXPECT_EQ(loader.Data(), nullptr);
  EXPECT_EQ(loader.Size(), 0u);
  // On Windows a mapped file cannot be deleted; after Unload it can.
  std::error_code ec;
  EXPECT_TRUE(std::filesystem::remove(path, ec)) << ec.message();
}

TEST(MmapModelLoaderTest, PreflightReportsWhetherTheFileMaps) {
  const auto path = TempFile("preflight.gguf", "GGUF payload");
  EXPECT_TRUE(azookey::host::PreflightModelMapping(path));
  EXPECT_FALSE(azookey::host::PreflightModelMapping(std::filesystem::temp_directory_path() /
                                                    "azookey_mmap_preflight_missing.gguf"));
  std::filesystem::remove(path);
}

TEST(MmapFallbackTest, MappedLoadThatSucceedsIsKept) {
  std::vector<bool> calls;
  MmapLoadReport report;
  const auto result = LoadWithMmapFallback(
      true, true,
      [&](bool use_mmap) {
        calls.push_back(use_mmap);
        return Attempt{true, use_mmap};
      },
      &report);
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(calls, std::vector<bool>({true}));
  EXPECT_TRUE(report.used_mmap);
  EXPECT_FALSE(report.fallback.has_value());
}

TEST(MmapFallbackTest, FailedMappedLoadRetriesOnceWithoutMmap) {
  std::vector<bool> calls;
  MmapLoadReport report;
  const auto result = LoadWithMmapFallback(
      true, true,
      [&](bool use_mmap) {
        calls.push_back(use_mmap);
        return Attempt{!use_mmap, use_mmap};
      },
      &report);
  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.use_mmap);
  EXPECT_EQ(calls, std::vector<bool>({true, false}));
  EXPECT_FALSE(report.used_mmap);
  EXPECT_EQ(report.fallback, MmapFallbackReason::Load);
}

TEST(MmapFallbackTest, BothLoadsFailingReturnsTheFailureWithoutAThirdTry) {
  std::vector<bool> calls;
  MmapLoadReport report;
  const auto result = LoadWithMmapFallback(
      true, true,
      [&](bool use_mmap) {
        calls.push_back(use_mmap);
        return Attempt{false, use_mmap};
      },
      &report);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(calls, std::vector<bool>({true, false}));
  EXPECT_EQ(report.fallback, MmapFallbackReason::Load);
}

TEST(MmapFallbackTest, FailedPreflightGoesStraightToAReadLoad) {
  std::vector<bool> calls;
  MmapLoadReport report;
  const auto result = LoadWithMmapFallback(
      true, false,
      [&](bool use_mmap) {
        calls.push_back(use_mmap);
        return Attempt{true, use_mmap};
      },
      &report);
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(calls, std::vector<bool>({false}));
  EXPECT_EQ(report.fallback, MmapFallbackReason::Preflight);
}

TEST(MmapFallbackTest, ReadLoadRequestedIsNotAFallback) {
  std::vector<bool> calls;
  MmapLoadReport report;
  (void)LoadWithMmapFallback(
      false, true,
      [&](bool use_mmap) {
        calls.push_back(use_mmap);
        return Attempt{true, use_mmap};
      },
      &report);
  EXPECT_EQ(calls, std::vector<bool>({false}));
  EXPECT_FALSE(report.used_mmap);
  EXPECT_FALSE(report.fallback.has_value());
}

}  // namespace
