#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "AppProfiles.h"
#include "SettingsDocument.h"
#include "azookey/ipc/Json.h"

namespace {

namespace j = azookey::ipc::json;
using azookey::settings::AppProfile;
using azookey::settings::AppProfiles;

class TempDirectory {
 public:
  TempDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("azookey_app_profiles_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
             std::to_string(next_id_++));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;
  std::filesystem::path File() const { return path_ / "settings.json"; }

 private:
  std::filesystem::path path_;
  static inline std::atomic<uint64_t> next_id_{0};
};

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

j::Object ReadRoot(const std::filesystem::path& path) {
  const auto parsed = j::Parse(ReadText(path));
  EXPECT_TRUE(parsed && parsed->IsObject());
  return parsed ? parsed->AsObject() : j::Object{};
}

AppProfile CodeProfile() {
  AppProfile profile;
  profile.profile_name = "Code";
  profile.prediction_enabled = true;
  profile.ai_backend = "local-zenzai";
  profile.prefer_technical_terms = true;
  profile.candidate_tag_boosts = {{"Technical", 1.5}, {"English", 1.3}};
  return profile;
}

}  // namespace

TEST(AppProfilesTest, ParsesOnlyTheFieldsThatAreSet) {
  const auto root = j::Parse(R"({
    "code.exe": {"profileName": "Code", "aiBackend": "openai", "promptPrefix": "",
                 "candidateTagBoosts": {"Technical": 1.5}},
    "mail.exe": {}
  })");
  ASSERT_TRUE(root);

  const auto profiles = azookey::settings::ParseAppProfiles(root->AsObject());

  ASSERT_EQ(profiles.size(), 2u);
  const auto& code = profiles.at("code.exe");
  EXPECT_EQ(code.profile_name, "Code");
  EXPECT_EQ(code.ai_backend, "openai");
  // An explicit empty prefix is kept as set: it clears the legacy value (section 6).
  ASSERT_TRUE(code.prompt_prefix);
  EXPECT_EQ(*code.prompt_prefix, "");
  EXPECT_FALSE(code.prediction_enabled);
  EXPECT_FALSE(code.privacy_mode);
  EXPECT_EQ(code.candidate_tag_boosts.at("Technical"), 1.5);
  EXPECT_EQ(profiles.at("mail.exe"), AppProfile{});
}

TEST(AppProfilesTest, SerializesSetFieldsAndRoundTrips) {
  AppProfiles profiles;
  profiles["Code.exe"] = CodeProfile();
  profiles["Code.exe"].prompt_prefix = "";
  profiles["default"].privacy_mode = "secure";

  const auto object = azookey::settings::AppProfilesToJson(profiles);

  const auto& code = object.at("Code.exe").AsObject();
  EXPECT_EQ(code.at("profileName").AsString(), "Code");
  EXPECT_EQ(code.at("promptPrefix").AsString(), "");
  EXPECT_FALSE(code.contains("learningEnabled"));
  EXPECT_FALSE(code.contains("style"));
  EXPECT_EQ(code.at("candidateTagBoosts").AsObject().at("English").AsNumber(), 1.3);
  EXPECT_EQ(object.at("default").AsObject().size(), 1u);
  EXPECT_EQ(azookey::settings::ParseAppProfiles(object), profiles);
}

TEST(AppProfilesTest, AnEmptyBoostMapIsLeftOut) {
  AppProfiles profiles;
  profiles["x.exe"].profile_name = "X";
  const auto object = azookey::settings::AppProfilesToJson(profiles);
  EXPECT_FALSE(object.at("x.exe").AsObject().contains("candidateTagBoosts"));
}

TEST(AppProfilesTest, ValidationRefusesWhatTheSchemaWouldNotAccept) {
  using azookey::settings::ValidateAppProfiles;
  AppProfiles profiles;
  profiles["code.exe"] = CodeProfile();
  EXPECT_FALSE(ValidateAppProfiles(profiles));

  auto bad = profiles;
  bad[""] = {};
  EXPECT_TRUE(ValidateAppProfiles(bad));

  bad = profiles;
  bad["CODE.EXE"] = {};
  EXPECT_TRUE(ValidateAppProfiles(bad)) << "keys differing only in case name the same app";

  bad = profiles;
  bad["code.exe"].ai_backend = "gpt";
  EXPECT_TRUE(ValidateAppProfiles(bad));
  bad = profiles;
  bad["code.exe"].style = "formal";
  EXPECT_TRUE(ValidateAppProfiles(bad));
  bad = profiles;
  bad["code.exe"].privacy_mode = "offline";
  EXPECT_TRUE(ValidateAppProfiles(bad)) << "offline is global only (section 4.2)";
  bad = profiles;
  bad["code.exe"].bracket_pairing = "maybe";
  EXPECT_TRUE(ValidateAppProfiles(bad));

  bad = profiles;
  bad["code.exe"].candidate_tag_boosts["Technical"] = 0.9;
  EXPECT_TRUE(ValidateAppProfiles(bad));
  bad["code.exe"].candidate_tag_boosts["Technical"] = 3.1;
  EXPECT_TRUE(ValidateAppProfiles(bad));
  bad["code.exe"].candidate_tag_boosts["Technical"] = 3.0;
  EXPECT_FALSE(ValidateAppProfiles(bad)) << "the bounds themselves are allowed";
  bad["code.exe"].candidate_tag_boosts["Technical"] = 1.0;
  EXPECT_FALSE(ValidateAppProfiles(bad));
  bad["code.exe"].candidate_tag_boosts[""] = 1.5;
  EXPECT_TRUE(ValidateAppProfiles(bad));
}

TEST(AppProfilesDocumentTest, LoadShowsProfilesAndTheReadOnlyLegacyPrefixes) {
  TempDirectory dir;
  WriteText(dir.File(), R"({
    "profilesByApp": {
      "code.exe": {"profileName": "Code", "style": "bogus",
                   "candidateTagBoosts": {"Technical": 9, "Bad": "x"}}
    },
    "promptPrefixByApp": {"Outlook.exe": "mail", "bad": false}
  })");

  const auto loaded = azookey::settings::LoadSettingsDocument(dir.File());

  ASSERT_TRUE(loaded.settings.profiles_by_app);
  const auto& code = loaded.settings.profiles_by_app->at("code.exe");
  EXPECT_EQ(code.profile_name, "Code");
  EXPECT_FALSE(code.style) << "an invalid field is dropped, not shown";
  // An out-of-range multiplier is clamped on load (section 4.2), a non-number is dropped.
  EXPECT_EQ(code.candidate_tag_boosts.at("Technical"), 3.0);
  EXPECT_FALSE(code.candidate_tag_boosts.contains("Bad"));
  EXPECT_EQ(loaded.settings.profiles_by_app, loaded.settings.profiles_by_app_loaded);
  ASSERT_EQ(loaded.settings.legacy_prompt_prefixes.size(), 1u);
  EXPECT_EQ(loaded.settings.legacy_prompt_prefixes.at("Outlook.exe"), "mail");
}

TEST(AppProfilesDocumentTest, ChangedProfilesAreWrittenAndLegacyPrefixesAreKept) {
  TempDirectory dir;
  WriteText(dir.File(), R"({
    "logLevel": "warn",
    "profilesByApp": {"old.exe": {"profileName": "Old"}},
    "promptPrefixByApp": {"Code.exe": "code editor"}
  })");
  auto loaded = azookey::settings::LoadSettingsDocument(dir.File());
  auto settings = loaded.settings;
  settings.profiles_by_app->erase("old.exe");
  (*settings.profiles_by_app)["code.exe"] = CodeProfile();

  const auto saved = azookey::settings::SaveSettingsDocument(dir.File(), settings);

  ASSERT_TRUE(saved.ok) << saved.error.value_or("");
  const auto root = ReadRoot(dir.File());
  const auto& profiles = root.at("profilesByApp").AsObject();
  EXPECT_EQ(profiles.size(), 1u);
  EXPECT_EQ(profiles.at("code.exe").AsObject().at("aiBackend").AsString(), "local-zenzai");
  EXPECT_EQ(root.at("promptPrefixByApp").AsObject().at("Code.exe").AsString(), "code editor");
  EXPECT_EQ(root.at("logLevel").AsString(), "warn");
  const auto reloaded = azookey::settings::LoadSettingsDocument(dir.File());
  EXPECT_EQ(reloaded.settings.profiles_by_app, settings.profiles_by_app);
}

TEST(AppProfilesDocumentTest, UntouchedProfilesAreNotRewritten) {
  TempDirectory dir;
  // Written out of the editor's order and with an empty map, to show the object is left as is.
  WriteText(dir.File(), R"({"profilesByApp": {"b.exe": {"candidateTagBoosts": {}}, "a.exe": {}}})");
  const auto loaded = azookey::settings::LoadSettingsDocument(dir.File());
  auto settings = loaded.settings;
  settings.log_level = "debug";

  const auto saved = azookey::settings::SaveSettingsDocument(dir.File(), settings);

  ASSERT_TRUE(saved.ok) << saved.error.value_or("");
  const auto root = ReadRoot(dir.File());
  const auto& profiles = root.at("profilesByApp").AsObject();
  EXPECT_TRUE(profiles.at("b.exe").AsObject().contains("candidateTagBoosts"));
}

TEST(AppProfilesDocumentTest, ASaveThatDoesNotCarryProfilesKeepsThemOnDisk) {
  TempDirectory dir;
  WriteText(dir.File(), R"({"profilesByApp": {"a.exe": {"profileName": "A"}}})");
  azookey::settings::EditableSettings settings;  // profiles_by_app unset

  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(dir.File(), settings).ok);

  EXPECT_EQ(ReadRoot(dir.File()).at("profilesByApp").AsObject().size(), 1u);
}

TEST(AppProfilesDocumentTest, DeletingEveryProfileWritesAnEmptyObject) {
  TempDirectory dir;
  WriteText(dir.File(), R"({"profilesByApp": {"a.exe": {"profileName": "A"}}})");
  auto settings = azookey::settings::LoadSettingsDocument(dir.File()).settings;
  settings.profiles_by_app->clear();

  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(dir.File(), settings).ok);

  EXPECT_TRUE(ReadRoot(dir.File()).at("profilesByApp").AsObject().empty());
}

TEST(AppProfilesDocumentTest, InvalidProfilesAreRefusedAndNothingIsWritten) {
  TempDirectory dir;
  const std::string original = R"({"logLevel": "warn", "profilesByApp": {"a.exe": {}}})";
  WriteText(dir.File(), original);
  auto settings = azookey::settings::LoadSettingsDocument(dir.File()).settings;
  (*settings.profiles_by_app)["code.exe"] = CodeProfile();
  (*settings.profiles_by_app)["code.exe"].candidate_tag_boosts["Technical"] = 100.0;
  settings.log_level = "debug";

  const auto saved = azookey::settings::SaveSettingsDocument(dir.File(), settings);

  EXPECT_FALSE(saved.ok);
  EXPECT_EQ(saved.invalid_setting, "profilesByApp");
  EXPECT_EQ(ReadText(dir.File()), original);
}
