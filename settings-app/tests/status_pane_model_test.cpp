#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "StatusPaneModel.h"

namespace {

using azookey::settings::ClassifyNeologdStatus;
using azookey::settings::NeologdStatusKind;

std::optional<azookey::ipc::NeologdLayerStatus> Layer(const char* state) {
  azookey::ipc::NeologdLayerStatus layer;
  layer.state = state;
  return layer;
}

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

TEST(NeologdStatusTest, ReportsWhatTheHostSays) {
  EXPECT_EQ(ClassifyNeologdStatus(Layer("ready"), true, true), NeologdStatusKind::Ready);
  EXPECT_EQ(ClassifyNeologdStatus(Layer("loading"), true, true), NeologdStatusKind::Loading);
  EXPECT_EQ(ClassifyNeologdStatus(Layer("missing_pack"), true, true),
            NeologdStatusKind::MissingPack);
  EXPECT_EQ(ClassifyNeologdStatus(Layer("error"), true, true), NeologdStatusKind::Error);
}

TEST(NeologdStatusTest, ANotRequestedLayerAfterConsentIsNotAFailure) {
  // The Host reads the pack only at startup, so a switch turned on later stays not_requested.
  EXPECT_EQ(ClassifyNeologdStatus(Layer("not_requested"), true, true),
            NeologdStatusKind::FetchAfterRestart);
  EXPECT_EQ(ClassifyNeologdStatus(Layer("not_requested"), false, true),
            NeologdStatusKind::FetchAfterSaveAndRestart);
  EXPECT_EQ(ClassifyNeologdStatus(Layer("not_requested"), false, false),
            NeologdStatusKind::NotEnabled);
  EXPECT_EQ(ClassifyNeologdStatus(Layer("not_requested"), true, false),
            NeologdStatusKind::NotEnabled);
}

TEST(NeologdStatusTest, AHostWithoutTheFieldOrWithAnUnknownStateIsUnknown) {
  EXPECT_EQ(ClassifyNeologdStatus(std::nullopt, true, true), NeologdStatusKind::Unknown);
  EXPECT_EQ(ClassifyNeologdStatus(Layer("something_new"), true, true), NeologdStatusKind::Unknown);
}

TEST(NeologdStatusTest, EveryKindHasAString) {
  const auto names = ResourceNames();
  for (const auto kind :
       {NeologdStatusKind::Unknown, NeologdStatusKind::NotEnabled,
        NeologdStatusKind::FetchAfterRestart, NeologdStatusKind::FetchAfterSaveAndRestart,
        NeologdStatusKind::Loading, NeologdStatusKind::Ready, NeologdStatusKind::MissingPack,
        NeologdStatusKind::Error}) {
    const auto name = azookey::settings::NeologdStatusResource(kind);
    EXPECT_TRUE(names.contains(name)) << name;
  }
}

TEST(PersonaFormatTest, FormatsRatiosAndTimes) {
  using namespace azookey::settings;
  EXPECT_EQ(FormatRatioPercent(0.452), "45.2%");
  EXPECT_EQ(FormatRatioPercent(0), "0.0%");
  EXPECT_EQ(FormatRatioPercent(1), "100.0%");
  EXPECT_EQ(FormatRatioPercent(-0.5), "0.0%");
  EXPECT_EQ(FormatRatioPercent(7), "100.0%");
  EXPECT_EQ(FormatPersonaTime(0), "");
  const auto time = FormatPersonaTime(1780000000);
  ASSERT_EQ(time.size(), 16u);
  EXPECT_EQ(time[4], '-');
  EXPECT_EQ(time[10], ' ');
  EXPECT_EQ(time[13], ':');
}

TEST(PersonaFormatTest, NoSamplesMeansNoData) {
  azookey::ipc::QueryPersonaResponse persona;
  EXPECT_FALSE(azookey::settings::PersonaHasData(persona));
  persona.sample_count = 1;
  EXPECT_TRUE(azookey::settings::PersonaHasData(persona));
}
