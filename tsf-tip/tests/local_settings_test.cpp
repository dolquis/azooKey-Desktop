#include <Windows.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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

TEST_F(LocalSettingsTest, ReloadsCommonProfilesAndPreservesPreviousSnapshot) {
  Write(R"({"bracketPairing":true,"profilesByApp":{"code.exe":{"bracketPairing":"on"}}})");
  ASSERT_TRUE(reader.Start(path));
  const auto previous = reader.Snapshot();
  const azookey::core::ForegroundApp app{"CODE.EXE", "Editor", true};
  ASSERT_TRUE(azookey::core::BracketPairingEnabledForApp(previous, app));
  Write(R"({"bracketPairing":true,"profilesByApp":{"code.exe":{"bracketPairing":"off"}}})");
  ASSERT_TRUE(reader.WaitForSnapshotForTest([&](const auto& settings) {
    return settings.pairing.enabled && settings.profiles &&
           settings.profiles->ResolveField("bracketPairing", app)->AsString() == "off" &&
           !azookey::core::BracketPairingEnabledForApp(settings, app);
  }));
  EXPECT_TRUE(azookey::core::BracketPairingEnabledForApp(previous, app));
  Write(R"({"bracketPairing":true,"profilesByApp":{"default":{"bracketPairing":"on"}}})");
  ASSERT_TRUE(reader.WaitForSnapshotForTest([&](const auto& settings) {
    return azookey::core::BracketPairingEnabledForApp(settings, app);
  }));
  Write(R"({"bracketPairing":true})");
  ASSERT_TRUE(reader.WaitForSnapshotForTest([&](const auto& settings) {
    return settings.pairing.enabled && settings.profiles &&
           settings.profiles->ResolveField("bracketPairing", app)->AsString() == "auto" &&
           !azookey::core::BracketPairingEnabledForApp(settings, app);
  }));
}

TEST_F(LocalSettingsTest, ReloadsTableCreationOverrideDisableAndDeletion) {
  Write(R"({"bracketPairing":true})");
  ASSERT_TRUE(reader.Start(path));
  const auto table = root / L"bracket-pairs.tsv";
  const auto expect_close = [&](char32_t close) {
    return reader.WaitForSnapshotForTest([&](const auto& settings) {
      const auto pair = azookey::core::LookupBracketPair(U'(', settings.Table());
      return close ? pair && pair->close == close : !pair;
    });
  };
  {
    std::ofstream stream(table);
    stream << "(\t}\ninvalid\n";
  }
  ASSERT_TRUE(expect_close(U'}'));
  {
    std::ofstream stream(table);
    stream << "(\t)\toff\n";
  }
  ASSERT_TRUE(expect_close(0));
  std::filesystem::remove(table);
  ASSERT_TRUE(expect_close(U')'));
}

TEST_F(LocalSettingsTest, ChangesWatchToCustomUnicodeDirectoryAndKeepsOldSnapshotImmutable) {
  Write(R"({"bracketPairing":true})");
  ASSERT_TRUE(reader.Start(path));
  const auto previous = reader.Snapshot();
  const auto custom = root / L"別設定" / L"pairs.tsv";
  std::filesystem::create_directories(custom.parent_path());
  {
    std::ofstream stream(custom);
    stream << "(\t}\n";
  }
  Write(R"({"bracketPairing":true,"bracketPairsPath":"別設定/pairs.tsv"})");
  const auto expect_close = [&](char32_t close) {
    return reader.WaitForSnapshotForTest([&](const auto& settings) {
      const auto pair = azookey::core::LookupBracketPair(U'(', settings.Table());
      return pair && pair->close == close;
    });
  };
  ASSERT_TRUE(expect_close(U'}'));
  {
    std::ofstream stream(custom);
    stream << "(\t]\n";
  }
  ASSERT_TRUE(expect_close(U']'));
  EXPECT_EQ(azookey::core::LookupBracketPair(U'(', previous.Table())->close, U')');
  Write(R"({"bracketPairing":true})");
  ASSERT_TRUE(expect_close(U')'));
}

TEST_F(LocalSettingsTest, WatchesOnlyNearestParentsForDefaultAndAbsoluteTablePaths) {
  Write(R"({"bracketPairing":true})");
  ASSERT_TRUE(reader.Start(path));
  auto directories = reader.WatchDirectoriesForTest();
  EXPECT_EQ(directories[0], path.parent_path());
  EXPECT_EQ(directories[1], root);
  reader.Stop();
  const auto custom = root / L"custom" / L"pairs.tsv";
  std::filesystem::create_directories(custom.parent_path());
  const auto utf8 = custom.generic_u8string();
  Write("{\"bracketPairsPath\":\"" + std::string(utf8.begin(), utf8.end()) + "\"}");
  ASSERT_TRUE(reader.Start(path));
  EXPECT_EQ(reader.WatchDirectoriesForTest()[1], custom.parent_path());
}

TEST_F(LocalSettingsTest, NestedUnrelatedFileWritesDoNotProduceWatchNotifications) {
  const auto nested = root / L"unrelated" / L"cache";
  std::filesystem::create_directories(nested);
  const auto file = nested / L"data.bin";
  {
    std::ofstream stream(file);
    stream << "initial";
  }
  Write(R"({"bracketPairing":true})");
  ASSERT_TRUE(reader.Start(path));
  const auto before = reader.WatchNotificationsForTest();
  {
    std::ofstream stream(file);
    stream << "changed cache contents";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(reader.WatchNotificationsForTest(), before);
}

TEST_F(LocalSettingsTest, RebindsFromMissingAncestorsWithoutRecursivelyWatchingTheirSiblings) {
  for (int iteration = 0; iteration < 20; ++iteration) {
    SCOPED_TRACE(iteration);
    path = root / (L"missing" + std::to_wstring(iteration)) / L"azooKey" / L"config" /
           L"settings.json";
    ASSERT_TRUE(reader.Start(path));
    EXPECT_EQ(reader.WatchDirectoriesForTest()[0], root);
    EXPECT_EQ(reader.WatchDirectoriesForTest()[1], root);
    Write(R"({"bracketPairing":true})");
    ASSERT_TRUE(reader.WaitForEnabledForTest(true));
    Write(R"({"bracketPairing":false})");
    ASSERT_TRUE(reader.WaitForEnabledForTest(false));
    reader.Stop();
  }
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
