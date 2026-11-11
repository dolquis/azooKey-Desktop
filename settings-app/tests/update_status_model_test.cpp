#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "UpdateStatusModel.h"

namespace {

using azookey::settings::ParseUpdateStatus;
using azookey::settings::UpdateCheckKind;
using azookey::settings::UpdateStatusResource;

std::set<std::string> ResourceNames() {
  std::ifstream input(std::filesystem::path(std::u8string(u8"" AZOOKEY_SETTINGS_RESW_PATH)),
                      std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const auto resw = buffer.str();
  std::set<std::string> names;
  const std::string marker = "<data name=\"";
  for (auto at = resw.find(marker); at != std::string::npos; at = resw.find(marker, at)) {
    at += marker.size();
    names.insert(resw.substr(at, resw.find('"', at) - at));
  }
  return names;
}

}  // namespace

// DEV-1934: the "バージョン" pane reads what the Host's UpdateChecker wrote.
TEST(UpdateStatusModelTest, ReadsTheHostRecord) {
  const auto status = ParseUpdateStatus(
      R"({"version":1,"checkedAtEpoch":1800000000,"channel":"stable","result":"update_available",)"
      R"("currentVersion":"1.0.0","latestVersion":"1.1.0",)"
      R"("releaseUrl":"https://github.com/dolquis/azooKey-Desktop/releases/tag/v1.1.0"})");
  EXPECT_EQ(status.kind, UpdateCheckKind::UpdateAvailable);
  EXPECT_EQ(status.channel, "stable");
  EXPECT_EQ(status.current_version, "1.0.0");
  EXPECT_EQ(status.latest_version, "1.1.0");
  EXPECT_EQ(status.release_url, "https://github.com/dolquis/azooKey-Desktop/releases/tag/v1.1.0");
  EXPECT_EQ(status.checked_at_epoch, 1800000000u);

  EXPECT_EQ(ParseUpdateStatus(R"({"version":1,"result":"up_to_date"})").kind,
            UpdateCheckKind::UpToDate);
  EXPECT_EQ(ParseUpdateStatus(R"({"version":1,"result":"failed"})").kind, UpdateCheckKind::Failed);
}

TEST(UpdateStatusModelTest, UnknownOrIncompleteRecordsShowNothing) {
  for (const char* json :
       {"", "{", "[]", R"({"result":"up_to_date"})", R"({"version":2,"result":"up_to_date"})",
        R"({"version":1,"result":"installed"})", R"({"version":1,"result":"update_available"})"}) {
    EXPECT_EQ(ParseUpdateStatus(json).kind, UpdateCheckKind::None) << json;
  }
  EXPECT_EQ(azookey::settings::ReadUpdateStatus(std::filesystem::temp_directory_path() /
                                                "azookey-missing-update-status.json")
                .kind,
            UpdateCheckKind::None);
}

TEST(UpdateStatusModelTest, OffersOnlyReleasePagesOfTheRepository) {
  for (const char* url :
       {"https://example.com/dolquis/azooKey-Desktop/releases/tag/v1.1.0",
        "http://github.com/dolquis/azooKey-Desktop/releases/tag/v1.1.0",
        "https://github.com/dolquis/azooKey-Desktop/releases/",
        "https://github.com/dolquis/azooKey-Desktop/releases/tag/v1 1", "javascript:alert(1)",
        "https://github.com/dolquis/azooKey-Desktop/releases/../../other/repo",
        "https://github.com/dolquis/azooKey-Desktop/releases/tag/%2e%2e"}) {
    const auto status = ParseUpdateStatus(
        std::string(R"({"version":1,"result":"update_available","latestVersion":"1.1.0",)") +
        R"("releaseUrl":")" + url + R"("})");
    EXPECT_EQ(status.kind, UpdateCheckKind::UpdateAvailable) << url;
    EXPECT_TRUE(status.release_url.empty()) << url;
  }
}

TEST(UpdateStatusModelTest, EverySentenceHasAString) {
  const auto names = ResourceNames();
  azookey::settings::UpdateStatusView status;
  status.channel = "stable";
  for (const auto kind : {UpdateCheckKind::None, UpdateCheckKind::UpdateAvailable,
                          UpdateCheckKind::UpToDate, UpdateCheckKind::Failed}) {
    status.kind = kind;
    const auto resource = UpdateStatusResource(status, true, false, "stable", "");
    EXPECT_TRUE(names.contains(resource)) << resource;
    // Turned off, or offline, the pane says so whatever an earlier check recorded.
    EXPECT_EQ(UpdateStatusResource(status, false, false, "stable", ""), "UpdateStatus_Disabled");
    EXPECT_EQ(UpdateStatusResource(status, true, true, "stable", ""), "UpdateStatus_Offline");
    // A record of the other channel is not shown as the result for this one.
    EXPECT_EQ(UpdateStatusResource(status, true, false, "beta", ""), "UpdateStatus_None");
  }
  EXPECT_TRUE(names.contains("UpdateStatus_Disabled"));
  EXPECT_TRUE(names.contains("UpdateStatus_Offline"));
}

// DEV-1934: right after an update, the previous build's record is not shown for this one.
TEST(UpdateStatusModelTest, ARecordOfAnotherProductVersionIsNotShown) {
  auto status = ParseUpdateStatus(
      R"({"version":1,"channel":"stable","result":"update_available","currentVersion":"1.0.0",)"
      R"("latestVersion":"1.1.0"})");
  ASSERT_EQ(status.kind, UpdateCheckKind::UpdateAvailable);
  EXPECT_EQ(UpdateStatusResource(status, true, false, "stable", "1.0.0"),
            "UpdateStatus_UpdateAvailable");
  EXPECT_EQ(UpdateStatusResource(status, true, false, "stable", "1.1.0"), "UpdateStatus_None");
  // The version could not be read: the record is shown as it is.
  EXPECT_EQ(UpdateStatusResource(status, true, false, "stable", ""),
            "UpdateStatus_UpdateAvailable");
}
