#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "LearningPaneModel.h"
#include "ModelPaneModel.h"

namespace {

using azookey::settings::HostCallStatus;
using azookey::settings::ModelEntryState;

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

// Resource names the pane sources refer to by literal, so a string missing from the resw (which
// the loader turns into an empty label) is caught here.
std::set<std::string> ResourcesUsedBy(const char* source) {
  std::ifstream input(
      std::filesystem::path(std::u8string(u8"" AZOOKEY_SETTINGS_SOURCE_DIR)) / source,
      std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const auto text = buffer.str();
  std::set<std::string> used;
  const std::regex pattern(
      R"(L"((?:Models|Learning|Profiles|HostCall|Persona|NeologdStatus|Proofread)_[A-Za-z0-9_]+)\")");
  for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
    used.insert((*it)[1].str());
  }
  return used;
}

azookey::ipc::ListedModel Model(const char* format, bool valid, const char* load_status) {
  azookey::ipc::ListedModel model;
  model.path = "C:\\models\\zenz.gguf";
  model.file_name = "zenz.gguf";
  model.format = format;
  model.valid = valid;
  model.last_load_status = load_status;
  return model;
}

}  // namespace

TEST(ModelPaneModelTest, ClassifiesAnEntryByValidityAndLoadHistory) {
  using azookey::settings::ClassifyModel;
  EXPECT_EQ(ClassifyModel(Model("gguf", true, "success")), ModelEntryState::Loaded);
  EXPECT_EQ(ClassifyModel(Model("gguf", true, "not_loaded")), ModelEntryState::NotLoaded);
  EXPECT_EQ(ClassifyModel(Model("gguf", true, "failed")), ModelEntryState::LoadFailed);
  EXPECT_EQ(ClassifyModel(Model("gguf", false, "success")), ModelEntryState::Invalid);
  // A valid ONNX GenAI entry is not shown as broken (model-management-spec section 6.2).
  EXPECT_EQ(ClassifyModel(Model("onnx_genai", true, "not_loaded")), ModelEntryState::NotLoaded);
}

TEST(ModelPaneModelTest, OnlyAValidGgufFileIsSelectable) {
  using azookey::settings::IsSelectableModel;
  EXPECT_TRUE(IsSelectableModel(Model("gguf", true, "not_loaded")));
  EXPECT_TRUE(IsSelectableModel(Model("gguf", true, "failed")));
  EXPECT_FALSE(IsSelectableModel(Model("gguf", false, "not_loaded")));
  EXPECT_FALSE(IsSelectableModel(Model("onnx_genai", true, "not_loaded")));
}

TEST(ModelPaneModelTest, ComparesPathsIgnoringCaseAndSeparators) {
  using azookey::settings::SameModelPath;
  EXPECT_TRUE(SameModelPath("C:\\Models\\Zenz.gguf", "c:/models/zenz.gguf"));
  EXPECT_TRUE(SameModelPath("C:\\models\\.\\zenz.gguf", "C:\\models\\zenz.gguf"));
  EXPECT_FALSE(SameModelPath("C:\\models\\a.gguf", "C:\\models\\b.gguf"));
  EXPECT_FALSE(SameModelPath("", ""));
}

TEST(ModelPaneModelTest, FormatsByteSizes) {
  using azookey::settings::FormatByteSize;
  EXPECT_EQ(FormatByteSize(0), "0 B");
  EXPECT_EQ(FormatByteSize(1023), "1023 B");
  EXPECT_EQ(FormatByteSize(1024), "1.0 KB");
  EXPECT_EQ(FormatByteSize(1288490189ULL), "1.2 GB");
  EXPECT_EQ(FormatByteSize(5ULL * 1024 * 1024 * 1024 * 1024), "5.0 TB");
}

TEST(ModelPaneModelTest, MapsHostErrorsToStringsAndFallsBackForUnknownOnes) {
  using azookey::settings::ModelErrorResource;
  EXPECT_EQ(ModelErrorResource("busy"), "ModelsError_busy");
  EXPECT_EQ(ModelErrorResource("not authenticated"), "ModelsError_not_authenticated");
  EXPECT_EQ(ModelErrorResource("something new"), "ModelsError_other");
}

TEST(ModelPaneModelTest, EveryErrorAndCallStatusHasAString) {
  const auto names = ResourceNames();
  for (const auto code : azookey::settings::kModelErrorCodes) {
    const auto name = azookey::settings::ModelErrorResource(code);
    EXPECT_TRUE(names.contains(name)) << name;
  }
  EXPECT_TRUE(names.contains("ModelsError_other"));
  for (const auto status : {HostCallStatus::Unavailable, HostCallStatus::HostNotRunning,
                            HostCallStatus::HandshakeRejected, HostCallStatus::Unsupported,
                            HostCallStatus::Timeout, HostCallStatus::InvalidResponse}) {
    const auto name = azookey::settings::HostCallStatusResource(status);
    EXPECT_TRUE(names.contains(name)) << name;
  }
}

TEST(LearningPaneModelTest, EveryErrorStoreAndConflictOptionHasAString) {
  const auto names = ResourceNames();
  for (const auto code : azookey::settings::kLearningErrorCodes) {
    const auto name = azookey::settings::LearningErrorResource(code);
    EXPECT_TRUE(names.contains(name)) << name;
  }
  EXPECT_TRUE(names.contains("LearningError_other"));
  EXPECT_EQ(azookey::settings::LearningErrorResource("brand_new"), "LearningError_other");
  for (const auto& tab : azookey::settings::kLearningStoreTabs) {
    EXPECT_TRUE(names.contains(std::string(tab.resource))) << tab.resource;
  }
  for (const auto& tab : azookey::settings::kLearningStoreTabs) {
    const auto name = "Learning_ResetNote_" + std::string(tab.id);
    EXPECT_TRUE(names.contains(name)) << name;
  }
  for (const auto option : azookey::settings::kConflictResolutions) {
    EXPECT_TRUE(names.contains("LearningConflict_" + std::string(option))) << option;
  }
}

TEST(LearningPaneModelTest, StoresMatchTheWireNames) {
  const auto& tabs = azookey::settings::kLearningStoreTabs;
  EXPECT_EQ(tabs[0].id, "learning");
  EXPECT_EQ(tabs[1].id, "user_dict");
  EXPECT_EQ(tabs[2].id, "typo");
  EXPECT_EQ(tabs[3].id, "auto_word");
}

TEST(LearningPaneModelTest, FormatsEntryColumns) {
  using namespace azookey::settings;
  EXPECT_EQ(FormatLearningDate(0), "");
  const auto date = FormatLearningDate(1780000000);
  ASSERT_EQ(date.size(), 10u);
  EXPECT_EQ(date[4], '-');
  EXPECT_EQ(date[7], '-');
  EXPECT_EQ(FormatLearningWeight(4.25), "4.2");
  EXPECT_EQ(FormatLearningWeight(3), "3.0");
  EXPECT_EQ(JoinLearningTags({}), "");
  EXPECT_EQ(JoinLearningTags({"learned", "Code.exe"}), "learned, Code.exe");
}

TEST(LearningPaneModelTest, ComputesThePageRange) {
  using azookey::settings::ComputeLearningPage;
  auto page = ComputeLearningPage(0, 100, 250);
  EXPECT_EQ(page.first, 1u);
  EXPECT_EQ(page.last, 100u);
  EXPECT_FALSE(page.has_previous);
  EXPECT_TRUE(page.has_next);
  page = ComputeLearningPage(200, 50, 250);
  EXPECT_EQ(page.first, 201u);
  EXPECT_EQ(page.last, 250u);
  EXPECT_TRUE(page.has_previous);
  EXPECT_FALSE(page.has_next);
  page = ComputeLearningPage(0, 0, 0);
  EXPECT_EQ(page.first, 0u);
  EXPECT_EQ(page.last, 0u);
  EXPECT_FALSE(page.has_previous);
  EXPECT_FALSE(page.has_next);
}

TEST(LearningPaneModelTest, AcceptsOnlyAnAbsoluteZipPathForABackup) {
  using azookey::settings::IsBackupArchivePath;
  EXPECT_TRUE(IsBackupArchivePath("C:\\Users\\me\\azookey-backup.zip"));
  EXPECT_TRUE(IsBackupArchivePath("C:\\Users\\me\\azookey-backup.ZIP"));
  EXPECT_FALSE(IsBackupArchivePath("azookey-backup.zip"));
  EXPECT_FALSE(IsBackupArchivePath("C:\\Users\\me\\azookey-backup.tar"));
  EXPECT_FALSE(IsBackupArchivePath("C:\\Users\\me\\"));
  EXPECT_FALSE(IsBackupArchivePath(""));
}

TEST(PaneSourcesTest, EveryStringTheNewPanesLoadIsInTheResources) {
  const auto names = ResourceNames();
  for (const char* source : {"ModelPane.cpp", "LearningPane.cpp", "ProfilesPane.cpp",
                             "PersonaPane.cpp", "ProofreadPane.cpp"}) {
    const auto used = ResourcesUsedBy(source);
    EXPECT_FALSE(used.empty()) << source;
    for (const auto& name : used) {
      EXPECT_TRUE(names.contains(name)) << source << ": " << name;
    }
  }
}
