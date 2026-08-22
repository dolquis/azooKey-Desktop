#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include "SettingsDocument.h"
#include "azookey/ipc/Json.h"
#include "azookey/learning/FileLock.h"

namespace {

std::filesystem::path TestDir(const char* name) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

void WriteText(const std::filesystem::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << content;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

}  // namespace

TEST(SettingsDocumentTest, MissingFileUsesM11Defaults) {
  const auto dir = TestDir("azookey_settings_document_missing");
  const auto result = azookey::settings::LoadSettingsDocument(dir / "settings.json");

  EXPECT_EQ(result.status, azookey::settings::SettingsDocumentStatus::Missing);
  EXPECT_TRUE(result.settings.model_enabled);
  ASSERT_TRUE(result.settings.model_backend_preference.has_value());
  EXPECT_EQ(*result.settings.model_backend_preference, "auto");
  EXPECT_TRUE(result.settings.model_selected_path.empty());
  EXPECT_EQ(result.settings.log_level, "info");
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, SavePreservesValidHiddenKeysAndDropsInvalidEntries) {
  const auto dir = TestDir("azookey_settings_document_writeback");
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "liveConversion": true,
    "aiBackend": "invalid-backend",
    "openAiModel": "local-test-model",
    "backendPreference": "cuda",
    "unknownRoot": 1,
    "promptPrefixByApp": {"Code.exe": "code", "bad": false},
    "model": {
      "enabled": true,
      "selectedPath": "old.gguf",
      "backendPreference": "cpu",
      "epPreference": "gpu",
      "unknownModel": true
    }
  })");

  azookey::settings::EditableSettings settings;
  settings.model_enabled = false;
  settings.model_backend_preference = "auto";
  settings.model_selected_path = "C:/models/new.gguf";
  settings.log_level = "debug";
  const auto saved = azookey::settings::SaveSettingsDocument(path, settings);

  ASSERT_TRUE(saved.ok) << saved.error.value_or("");
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto& root = parsed->AsObject();
  EXPECT_TRUE(root.contains("liveConversion"));
  EXPECT_TRUE(root.contains("openAiModel"));
  EXPECT_FALSE(root.contains("aiBackend"));
  EXPECT_FALSE(root.contains("unknownRoot"));
  EXPECT_FALSE(root.contains("backendPreference"));
  ASSERT_TRUE(root.contains("promptPrefixByApp"));
  EXPECT_EQ(root.at("promptPrefixByApp").AsObject().size(), 1u);

  ASSERT_TRUE(root.contains("model"));
  const auto& model = root.at("model").AsObject();
  EXPECT_FALSE(model.at("enabled").AsBool());
  EXPECT_EQ(model.at("selectedPath").AsString(), "C:/models/new.gguf");
  EXPECT_EQ(model.at("backendPreference").AsString(), "auto");
  EXPECT_EQ(model.at("epPreference").AsString(), "gpu");
  EXPECT_FALSE(model.contains("unknownModel"));
  EXPECT_EQ(root.at("logLevel").AsString(), "debug");
  EXPECT_FALSE(saved.warnings.empty());
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, UnsupportedValidBackendIsPreservedUntilUserSelectsOne) {
  const auto dir = TestDir("azookey_settings_document_hidden_backend");
  const auto path = dir / "settings.json";
  WriteText(path, R"({"model":{"backendPreference":"winml"}})");

  const auto loaded = azookey::settings::LoadSettingsDocument(path);
  ASSERT_EQ(loaded.status, azookey::settings::SettingsDocumentStatus::Loaded);
  EXPECT_FALSE(loaded.settings.model_backend_preference.has_value());
  EXPECT_EQ(loaded.settings.hidden_backend_preference, "winml");

  auto edited = loaded.settings;
  edited.log_level = "warn";
  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, edited).ok);
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  EXPECT_EQ(parsed->AsObject().at("model").AsObject().at("backendPreference").AsString(), "winml");
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, LockFailureLeavesExistingFileUntouched) {
  const auto dir = TestDir("azookey_settings_document_lock_failure");
  const auto path = dir / "settings.json";
  const std::string original = R"({"logLevel":"info"})";
  WriteText(path, original);

  std::promise<bool> acquired;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::thread holder([&] {
    auto lock =
        azookey::learning::AcquireExclusiveFileLockForPath(path, std::chrono::milliseconds(1000));
    acquired.set_value(lock.has_value());
    if (lock) release_future.wait();
  });
  const bool has_lock = acquired.get_future().get();
  if (!has_lock) {
    release.set_value();
    holder.join();
  }
  ASSERT_TRUE(has_lock);

  azookey::settings::EditableSettings settings;
  settings.log_level = "debug";
  const auto result =
      azookey::settings::SaveSettingsDocument(path, settings, std::chrono::milliseconds(20));

  release.set_value();
  holder.join();
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(ReadText(path), original);
  std::filesystem::remove_all(dir);
}

#ifdef _WIN32
TEST(SettingsDocumentTest, ReadFailureLeavesExistingFileUntouched) {
  const auto dir = TestDir("azookey_settings_document_read_failure");
  const auto path = dir / "settings.json";
  const std::string original = R"({"logLevel":"info"})";
  WriteText(path, original);

  HANDLE exclusive = CreateFileW(path.wstring().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(exclusive, INVALID_HANDLE_VALUE);

  azookey::settings::EditableSettings settings;
  settings.log_level = "debug";
  const auto result = azookey::settings::SaveSettingsDocument(path, settings);

  CloseHandle(exclusive);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(ReadText(path), original);
  std::filesystem::remove_all(dir);
}
#endif

TEST(SettingsDocumentTest, RejectsInvalidEditableEnums) {
  const auto dir = TestDir("azookey_settings_document_invalid_editable_enums");
  const auto path = dir / "settings.json";

  azookey::settings::EditableSettings settings;
  settings.log_level = "verbose";
  EXPECT_FALSE(azookey::settings::SaveSettingsDocument(path, settings).ok);

  settings.log_level = "info";
  settings.model_backend_preference = "winml";
  EXPECT_FALSE(azookey::settings::SaveSettingsDocument(path, settings).ok);
  EXPECT_FALSE(std::filesystem::exists(path));
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, InvalidDocumentCanBeRecoveredByAtomicSave) {
  const auto dir = TestDir("azookey_settings_document_invalid");
  const auto path = dir / "settings.json";
  WriteText(path, "{ invalid json");

  azookey::settings::EditableSettings settings;
  settings.model_enabled = false;
  const auto result = azookey::settings::SaveSettingsDocument(path, settings);

  ASSERT_TRUE(result.ok);
  EXPECT_FALSE(result.warnings.empty());
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  EXPECT_FALSE(parsed->AsObject().at("model").AsObject().at("enabled").AsBool());
  std::filesystem::remove_all(dir);
}
