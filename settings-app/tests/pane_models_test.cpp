#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "AppProfiles.h"
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
  const std::regex pattern(R"(L"((?:Models|Learning|Profiles|HostCall|Persona|NeologdStatus|)"
                           R"(Proofread|SaveBar)_[A-Za-z0-9_]+)\")");
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

TEST(ModelPaneModelTest, AFileInBothListsIsChosenFromTheModelsListOnly) {
  using azookey::settings::IsCatalogEntryListedInModels;
  using azookey::settings::IsDefaultCatalogModel;
  const std::vector<azookey::ipc::ListedModel> models = {Model("gguf", true, "")};
  azookey::ipc::CatalogModelEntry entry;
  entry.id = "zenz";
  entry.is_default = true;
  entry.state = "verified";
  // Only case and separators differ from the listed path.
  entry.path = "c:/Models/ZENZ.gguf";
  EXPECT_TRUE(IsCatalogEntryListedInModels(entry, models));
  EXPECT_TRUE(IsDefaultCatalogModel(models[0], {entry}));
  // A file that is not there yet, or another file of the same name, stays a catalog row.
  for (const char* state : {"missing", "mismatch"}) {
    SCOPED_TRACE(state);
    auto other = entry;
    other.state = state;
    EXPECT_FALSE(IsCatalogEntryListedInModels(other, models));
    EXPECT_FALSE(IsDefaultCatalogModel(models[0], {other}));
  }
  auto elsewhere = entry;
  elsewhere.path = "C:\\models\\zenzai\\other.gguf";
  EXPECT_FALSE(IsCatalogEntryListedInModels(elsewhere, models));
  EXPECT_FALSE(IsCatalogEntryListedInModels(entry, {}));
  auto not_default = entry;
  not_default.is_default = false;
  EXPECT_FALSE(IsDefaultCatalogModel(models[0], {not_default}));
}

TEST(ModelPaneModelTest, ShowsTheChosenPathOrThatNoneIsChosen) {
  using azookey::settings::SelectedModelDisplay;
  EXPECT_EQ(SelectedModelDisplay("", L"none"), L"none");
  EXPECT_EQ(SelectedModelDisplay("C:\\models\\zenz.gguf", L"none"), L"C:\\models\\zenz.gguf");
  // UTF-8 bytes of a folder named "モデル", read as UTF-8 and not in the ANSI code page.
  EXPECT_EQ(SelectedModelDisplay("C:\\\xE3\x83\xA2\xE3\x83\x87\xE3\x83\xAB\\a.gguf", L"none"),
            L"C:\\モデル\\a.gguf");
}

TEST(ModelPaneModelTest, OnlyAnExistingAbsoluteGgufFileIsUsable) {
  using azookey::settings::IsUsableModelFile;
  const auto dir = std::filesystem::temp_directory_path() / "azookey_settings_usable_model";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "folder.gguf");
  for (const char* name : {"model.gguf", "UPPER.GGUF", "model.bin"}) {
    std::ofstream(dir / name, std::ios::binary) << "GGUF";
  }
  EXPECT_TRUE(IsUsableModelFile(dir / "model.gguf"));
  EXPECT_TRUE(IsUsableModelFile(dir / "UPPER.GGUF"));
  EXPECT_FALSE(IsUsableModelFile(dir / "model.bin"));
  EXPECT_FALSE(IsUsableModelFile(dir / "missing.gguf"));
  EXPECT_FALSE(IsUsableModelFile(dir / "folder.gguf"));
  EXPECT_FALSE(IsUsableModelFile(std::filesystem::path("model.gguf")));
  EXPECT_FALSE(IsUsableModelFile(std::filesystem::path()));
  std::filesystem::remove_all(dir);
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

// DEV-1821 catalog fetch.
TEST(ModelPaneModelTest, EveryCatalogFetchErrorResultAndStateHasAString) {
  using azookey::settings::CatalogFetchErrorResource;
  const auto names = ResourceNames();
  for (const auto code : azookey::settings::kCatalogFetchErrorCodes) {
    const auto name = CatalogFetchErrorResource(code);
    EXPECT_TRUE(names.contains(name)) << name;
  }
  EXPECT_TRUE(names.contains("CatalogFetchError_other"));
  EXPECT_EQ(CatalogFetchErrorResource("format:magic_mismatch"), "CatalogFetchError_format");
  EXPECT_EQ(CatalogFetchErrorResource("not authenticated"), "CatalogFetchError_not_authenticated");
  EXPECT_EQ(CatalogFetchErrorResource("brand_new"), "CatalogFetchError_other");
  azookey::ipc::CatalogFetchResponse response;
  response.state = "done";
  for (const auto* status : {"downloaded", "already_valid", "offline", "not_fetchable"}) {
    response.status = status;
    EXPECT_TRUE(names.contains(azookey::settings::CatalogFetchResultResource(response))) << status;
  }
  response.status = "failed";
  response.result_error = "destination_occupied";
  EXPECT_EQ(azookey::settings::CatalogFetchResultResource(response),
            "CatalogFetchError_destination_occupied");
  for (const auto* state : {"missing", "verified", "mismatch"}) {
    EXPECT_TRUE(names.contains(azookey::settings::CatalogEntryStateResource(state))) << state;
  }
}

TEST(ModelPaneModelTest, OnlyADoneFetchWithAVerifiedFileCanBeSelected) {
  using azookey::settings::CatalogFetchSucceeded;
  azookey::ipc::CatalogFetchResponse response;
  response.state = "done";
  response.status = "downloaded";
  response.path = "C:\\models\\zenzai\\zenz.gguf";
  EXPECT_TRUE(CatalogFetchSucceeded(response));
  response.status = "already_valid";
  EXPECT_TRUE(CatalogFetchSucceeded(response));
  response.status = "offline";
  EXPECT_FALSE(CatalogFetchSucceeded(response));
  response.status = "downloaded";
  response.state = "running";
  EXPECT_FALSE(CatalogFetchSucceeded(response));
  response.state = "done";
  response.path.clear();
  EXPECT_FALSE(CatalogFetchSucceeded(response));
}

// DEV-1840: no fetch starts while a stop this window sent is unanswered, since the Host
// matches the stop by model id and could apply it to that new fetch.
TEST(ModelPaneModelTest, AFetchStartsOnlyForAMissingEntryWhileNothingRunsOrIsBeingStopped) {
  using azookey::settings::CanStartCatalogFetch;
  azookey::ipc::CatalogModelEntry entry;
  entry.id = "zenz";
  entry.fetchable = true;
  entry.state = "missing";
  EXPECT_TRUE(CanStartCatalogFetch(entry, false, false));
  EXPECT_FALSE(CanStartCatalogFetch(entry, true, false));
  EXPECT_FALSE(CanStartCatalogFetch(entry, false, true));
  entry.state = "mismatch";
  EXPECT_FALSE(CanStartCatalogFetch(entry, false, false));
  entry.state = "missing";
  entry.fetchable = false;
  EXPECT_FALSE(CanStartCatalogFetch(entry, false, false));
}

TEST(ModelPaneModelTest, ProgressPercentIsCappedAndZeroWithoutATotal) {
  using azookey::settings::ProgressPercent;
  EXPECT_EQ(ProgressPercent(0, 0), 0U);
  EXPECT_EQ(ProgressPercent(50, 0), 0U);
  EXPECT_EQ(ProgressPercent(1, 200), 0U);
  EXPECT_EQ(ProgressPercent(100, 200), 50U);
  // Not 100 until every byte is in: the Host still checks the file after the last one.
  EXPECT_EQ(ProgressPercent(199, 200), 99U);
  EXPECT_EQ(ProgressPercent(200, 200), 100U);
  EXPECT_EQ(ProgressPercent(300, 200), 100U);
  EXPECT_EQ(ProgressPercent(UINT64_MAX - 1, UINT64_MAX), 99U);
}

TEST(ModelPaneModelTest, OnlyTimeWithoutNewBytesCountsAsAStall) {
  using Clock = azookey::settings::CatalogFetchStallWatch::Clock;
  const auto start = Clock::time_point{};
  azookey::settings::CatalogFetchStallWatch watch(start, std::chrono::seconds(10));
  EXPECT_FALSE(watch.Stalled(0, start + std::chrono::seconds(9)));
  // Bytes keep arriving, so a long download is never a stall.
  EXPECT_FALSE(watch.Stalled(100, start + std::chrono::seconds(30)));
  EXPECT_FALSE(watch.Stalled(200, start + std::chrono::seconds(60)));
  EXPECT_FALSE(watch.Stalled(200, start + std::chrono::seconds(70)));
  EXPECT_TRUE(watch.Stalled(200, start + std::chrono::seconds(71)));
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

TEST(LearningPaneModelTest, EveryUserDictErrorHasAString) {
  const auto names = ResourceNames();
  for (const auto code : azookey::settings::kUserDictErrorCodes) {
    const auto name = azookey::settings::UserDictErrorResource(code);
    EXPECT_TRUE(names.contains(name)) << name;
  }
  EXPECT_TRUE(names.contains("LearningUserDictError_other"));
  EXPECT_EQ(azookey::settings::UserDictErrorResource("brand_new"), "LearningUserDictError_other");
  EXPECT_EQ(azookey::settings::UserDictErrorResource(""), "LearningUserDictError_other");
}

TEST(LearningPaneModelTest, UserWordInputIsTrimmedAndTheReadingFoldedToHiragana) {
  using azookey::settings::NormalizeUserWordInput;
  using azookey::settings::UserWordInputError;
  // "\xE3\x80\x80" is U+3000; "\xE3\x82\xA2\xE3\x83\xB6" is katakana A and KE (U+30A2, U+30F6);
  // "\xE3\x81\x82\xE3\x82\x96" is hiragana a and small ke (U+3042, U+3096).
  const auto input = NormalizeUserWordInput(" \xE3\x80\x80\xE3\x82\xA2\xE3\x83\xB6\t",
                                            "\xE3\x80\x80 azooKey \xE3\x80\x80");
  EXPECT_EQ(input.error, UserWordInputError::None);
  EXPECT_EQ(input.reading, "\xE3\x81\x82\xE3\x82\x96");
  EXPECT_EQ(input.word, "azooKey");

  // The long vowel mark (U+30FC) and the middle dot (U+30FB) are outside the fold.
  EXPECT_EQ(NormalizeUserWordInput("\xE3\x83\xBC\xE3\x83\xBB", "x").reading,
            "\xE3\x83\xBC\xE3\x83\xBB");
  // The word keeps its katakana.
  EXPECT_EQ(NormalizeUserWordInput("a", "\xE3\x82\xA2").word, "\xE3\x82\xA2");

  EXPECT_EQ(NormalizeUserWordInput(" \xE3\x80\x80", "azooKey").error,
            UserWordInputError::EmptyReading);
  EXPECT_EQ(NormalizeUserWordInput("azuki", "\t").error, UserWordInputError::EmptyWord);
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

namespace {

std::string ReadSource(const char* name) {
  std::ifstream input(std::filesystem::path(std::u8string(u8"" AZOOKEY_SETTINGS_SOURCE_DIR)) / name,
                      std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

// Whether a UTF-8 string holds a pictograph: any code point outside the BMP, the dingbats and
// miscellaneous symbols (U+2600-U+27BF), or the emoji presentation selector U+FE0F.
bool HasEmoji(const std::string& text) {
  for (size_t at = 0; at < text.size(); ++at) {
    const auto lead = static_cast<unsigned char>(text[at]);
    if (lead >= 0xF0) return true;
    if (at + 2 >= text.size()) continue;
    const auto second = static_cast<unsigned char>(text[at + 1]);
    const auto third = static_cast<unsigned char>(text[at + 2]);
    if (lead == 0xE2 && second >= 0x98 && second <= 0x9E) return true;
    if (lead == 0xEF && second == 0xB8 && third == 0x8F) return true;
  }
  return false;
}

}  // namespace

TEST(ReswNotationTest, NoStringShowsAnInternalWordOrAnEmoji) {
  // sideload-packaging-spec section 3.2, "表記規則".
  std::ifstream input(std::filesystem::path(std::u8string(u8"" AZOOKEY_SETTINGS_RESW_PATH)),
                      std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const auto resw = buffer.str();
  const std::vector<std::string> internal_words = {
      "Host",    "backend",   "catalog", "pack",         "prompt prefix",
      "Persona", "Settings ", "merge",   "overwrite",    "keep_both",
      "bytes",   "v1.",       "括弧",    "バックエンド", "整文"};
  const std::string name_marker = "<data name=\"";
  const std::string open = "<value>";
  const std::string close = "</value>";
  size_t checked = 0;
  for (auto at = resw.find(name_marker); at != std::string::npos; at = resw.find(name_marker, at)) {
    at += name_marker.size();
    const auto name = resw.substr(at, resw.find('"', at) - at);
    const auto begin = resw.find(open, at) + open.size();
    const auto value = resw.substr(begin, resw.find(close, begin) - begin);
    ++checked;
    for (const auto& word : internal_words) {
      EXPECT_EQ(value.find(word), std::string::npos) << name << ": " << word;
    }
    EXPECT_FALSE(HasEmoji(value)) << name;
  }
  EXPECT_GT(checked, 100u);
}

TEST(ReswNotationTest, EveryUidInTheWindowHasAStringAndNoPageWideHeadingIsLeft) {
  const auto names = ResourceNames();
  const auto xaml = ReadSource("MainWindow.xaml");
  const std::regex pattern(R"re(x:Uid="([A-Za-z0-9_]+)")re");
  size_t uids = 0;
  for (std::sregex_iterator it(xaml.begin(), xaml.end(), pattern), end; it != end; ++it) {
    const auto uid = (*it)[1].str();
    ++uids;
    const auto prefix = uid + ".";
    const auto found = names.lower_bound(prefix);
    EXPECT_TRUE(found != names.end() && found->starts_with(prefix)) << uid;
  }
  EXPECT_GT(uids, 20u);
  // Each pane carries its own heading and line; the app-wide ones are gone (section 3.2).
  EXPECT_EQ(xaml.find("PageTitle"), std::string::npos);
  EXPECT_EQ(xaml.find("PageDescription"), std::string::npos);
}

TEST(ReswNotationTest, EnumValuesShowAStringRatherThanTheSavedValue) {
  using namespace azookey::settings;
  const auto names = ResourceNames();
  const auto check = [&names](const auto& options) {
    for (const auto option : options) {
      EXPECT_TRUE(names.contains(ProfileOptionResource(option))) << option;
    }
  };
  check(kProfileAiBackends);
  check(kProfileStyles);
  check(kProfilePrivacyModes);
  check(kProfileBracketPairings);
  EXPECT_EQ(ProfileOptionResource("local-zenzai"), "Profiles_Option_local_zenzai");
  for (const char* status : {"success", "timeout", "error", "unexpected"}) {
    EXPECT_TRUE(names.contains(BenchmarkStatusResource(status))) << status;
  }
  EXPECT_EQ(BenchmarkStatusResource("unexpected"), "Models_BenchStatus_other");
  EXPECT_EQ(InferenceDeviceName("cpu"), "CPU");
  EXPECT_EQ(InferenceDeviceName("vulkan"), "Vulkan");
  EXPECT_EQ(InferenceDeviceName("npu"), "npu");
}

TEST(PaneSourcesTest, EveryStringTheNewPanesLoadIsInTheResources) {
  const auto names = ResourceNames();
  for (const char* source : {"ModelPane.cpp", "LearningPane.cpp", "ProfilesPane.cpp",
                             "PersonaPane.cpp", "ProofreadPane.cpp", "MainWindow.xaml.cpp"}) {
    const auto used = ResourcesUsedBy(source);
    EXPECT_FALSE(used.empty()) << source;
    for (const auto& name : used) {
      EXPECT_TRUE(names.contains(name)) << source << ": " << name;
    }
  }
}
