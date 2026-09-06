#include <Windows.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include "azookey/tsf/TipLocalSettings.h"

namespace {
class LocalSettingsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<unsigned> sequence{0};
    root = std::filesystem::temp_directory_path() /
           (L"azookey-bracket-settings-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++));
    std::filesystem::create_directories(root);
    path = root / L"config" / L"settings.json";
  }
  void TearDown() override {
    reader.Stop();
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  void Write(const std::string& json) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << json;
    stream.close();
    ASSERT_TRUE(stream.good());
  }
  std::filesystem::path root;
  std::filesystem::path path;
  azookey::tsf::TipLocalSettings reader;
};

TEST_F(LocalSettingsTest, LoadsSharedFileWithoutHostAndStopsIdempotently) {
  Write(R"({"bracketPairing":true,"bracketPairingTrigger":"composition"})");
  ASSERT_TRUE(reader.Start(path));
  EXPECT_TRUE(reader.Snapshot().pairing.enabled);
  EXPECT_EQ(reader.Snapshot().trigger, azookey::core::BracketPairingTrigger::Composition);
  reader.Stop();
  reader.Stop();
}

TEST_F(LocalSettingsTest, DetectsCreationModificationDeletionAndReplacement) {
  ASSERT_TRUE(reader.Start(path));
  EXPECT_FALSE(reader.Snapshot().pairing.enabled);
  Write(R"({"bracketPairing":true})");
  ASSERT_TRUE(reader.WaitForEnabledForTest(true));
  Write(R"({"bracketPairing":false})");
  ASSERT_TRUE(reader.WaitForEnabledForTest(false));
  Write(R"({"bracketPairing":true})");
  ASSERT_TRUE(reader.WaitForEnabledForTest(true));
  ASSERT_TRUE(std::filesystem::remove(path));
  ASSERT_TRUE(reader.WaitForEnabledForTest(false));
  auto temporary = path;
  temporary += L".tmp";
  {
    std::ofstream stream(temporary);
    stream << R"({"bracketPairing":true})";
  }
  std::filesystem::rename(temporary, path);
  ASSERT_TRUE(reader.WaitForEnabledForTest(true));
}

TEST_F(LocalSettingsTest, InvalidOrOversizedFileRestoresDisabledDefaults) {
  Write(R"({"bracketPairing":true})");
  ASSERT_TRUE(reader.Start(path));
  EXPECT_TRUE(reader.Snapshot().pairing.enabled);
  Write("{");
  ASSERT_TRUE(reader.WaitForEnabledForTest(false));
  Write(R"({"bracketPairing":true})");
  ASSERT_TRUE(reader.WaitForEnabledForTest(true));
  Write(std::string(1024 * 1024 + 1, ' ') + R"({"bracketPairing":true})");
  ASSERT_TRUE(reader.WaitForEnabledForTest(false));
}

TEST_F(LocalSettingsTest, WatchesUnicodePathsAndRecreatedConfigDirectory) {
  path = root / L"設定" / L"settings.json";
  Write(R"({"bracketPairing":true})");
  ASSERT_TRUE(reader.Start(path));
  ASSERT_TRUE(reader.Snapshot().pairing.enabled);
  std::filesystem::remove(path);
  std::filesystem::remove(path.parent_path());
  ASSERT_TRUE(reader.WaitForEnabledForTest(false));
  Write(R"({"bracketPairing":true})");
  ASSERT_TRUE(reader.WaitForEnabledForTest(true));
}
}  // namespace
