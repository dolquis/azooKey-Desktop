#include <Windows.h>
#include <gtest/gtest.h>

#ifdef GetObject
#undef GetObject
#endif

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "azookey/ipc/Json.h"
#include "runner/CaseSupport.h"
#include "runner/ClipboardIsolation.h"
#include "runner/ReportWriter.h"
#include "runner/ScreenshotCapture.h"
#include "runner/TargetConfigLoader.h"
#include "runner/WindowOwnership.h"

namespace azookey::compat_test {
namespace {

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!stream) throw std::runtime_error("failed to write test file");
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

constexpr std::string_view kTargetConfigJson = R"json({
  "id": "test-target",
  "display_name": "Test target",
  "app_id": "Test.App",
  "automation_level": "full",
  "launch": {
    "executable": "test.exe",
    "close_grace_period_ms": 5000,
    "temporary_document": {
      "extension": ".html",
      "contents": "<textarea aria-label=\"compat editor\"></textarea>"
    }
  },
  "window": {
    "class": "TestWindow",
    "edit_control_class": "TestEditor",
    "editor_control_type": "edit",
    "editor_name": "compat editor",
    "candidate_window_class": "CandidateWindow"
  },
  "cases": ["C-001"],
  "workarounds": {
    "require_new_window": false,
    "use_temporary_document": false,
    "allow_reused_window_for_temporary_document": true
  }
})json";

std::optional<TargetConfig> LoadTargetConfigText(std::string_view contents) {
  TemporaryDirectory temp;
  const auto path = temp.path() / "target.json";
  WriteFile(path, contents);
  return LoadTargetConfig(path);
}

class FakeClipboardAccess final : public ClipboardAccess {
 public:
  bool Backup() override {
    calls.push_back("backup");
    return backup_succeeds;
  }
  bool ReplaceWithDeterministicText(std::wstring_view text) override {
    calls.push_back("replace");
    replacement.assign(text);
    return replacement_succeeds;
  }
  bool Restore() override {
    calls.push_back("restore");
    restored = restore_succeeds;
    return restore_succeeds;
  }

  std::vector<std::string> calls;
  std::wstring replacement;
  bool backup_succeeds{true};
  bool replacement_succeeds{true};
  bool restore_succeeds{true};
  bool restored{false};
};

TEST(C002BackspaceObservationTest, AcceptsExpectedTransitionsForBothBatchModes) {
  EXPECT_TRUE(IsExpectedC002BackspaceTransition(C002BackspaceScenario::kBefore,
                                                C002BackspaceScenario::kExpectedAfter[0]));
  EXPECT_TRUE(IsExpectedC002BackspaceTransition(C002BackspaceScenario::kBefore,
                                                C002BackspaceScenario::kExpectedAfter[1]));
}

TEST(C002BackspaceObservationTest, RejectsUnchangedShortenedAndUnexpectedTransitions) {
  EXPECT_FALSE(IsExpectedC002BackspaceTransition(C002BackspaceScenario::kBefore,
                                                 C002BackspaceScenario::kBefore));
  EXPECT_FALSE(IsExpectedC002BackspaceTransition(C002BackspaceScenario::kBefore, L"にほ"));
  EXPECT_FALSE(IsExpectedC002BackspaceTransition(C002BackspaceScenario::kBefore, L"にほんk"));
}

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
  EXPECT_NE(markdown.find("<!-- azookey-compat-report:notepad -->"), std::string::npos);
  EXPECT_NE(markdown.find("**Outcome: FAIL**"), std::string::npos);
  EXPECT_NE(markdown.find("<summary>Case details</summary>"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(temp.path() / "logs"));
}

TEST(TargetConfigFilesTest, M50GateTargetsDeclareFullAutomationContract) {
  struct ExpectedTarget {
    const char* id;
    const char* app_id;
    const char* window_class;
    const char* editor_control_type;
    const char* editor_name;
  };
  constexpr std::array targets{
      ExpectedTarget{"notepad", "Microsoft.WindowsNotepad_8wekyb3d8bbwe!App", "Notepad", "document",
                     nullptr},
      ExpectedTarget{"vscode", "Microsoft.VisualStudioCode", "Chrome_WidgetWin_1", "document",
                     nullptr},
      ExpectedTarget{"edge", "MSEdge", "Chrome_WidgetWin_1", "edit",
                     "azooKey compatibility editor"},
  };
  const std::set<std::string> expected_cases{
      "C-001", "C-002", "C-003", "C-004", "C-005", "C-006",
      "C-007", "C-008", "C-009", "C-010", "C-011", "C-012",
  };

  for (const auto& expected : targets) {
    SCOPED_TRACE(expected.id);
    const auto path =
        std::filesystem::path(AZOOKEY_COMPAT_TARGETS_DIR) / (std::string(expected.id) + ".json");
    const auto parsed = azookey::ipc::json::Parse(ReadFile(path));
    ASSERT_TRUE(parsed);
    ASSERT_TRUE(parsed->IsObject());
    EXPECT_EQ(parsed->GetString("id"), expected.id);
    EXPECT_EQ(parsed->GetString("app_id"), expected.app_id);
    EXPECT_EQ(parsed->GetString("automation_level"), "full");

    const auto* window = parsed->GetObject("window");
    ASSERT_NE(window, nullptr);
    const azookey::ipc::json::Value window_value(*window);
    EXPECT_EQ(window_value.GetString("class"), expected.window_class);
    EXPECT_EQ(window_value.GetString("editor_control_type"), expected.editor_control_type);
    if (expected.editor_name) {
      EXPECT_EQ(window_value.GetString("editor_name"), expected.editor_name);
    } else {
      EXPECT_FALSE(window_value.GetString("editor_name").has_value());
    }

    const auto* cases = parsed->GetArray("cases");
    ASSERT_NE(cases, nullptr);
    std::set<std::string> actual_cases;
    for (const auto& item : *cases) {
      ASSERT_TRUE(item.IsString());
      actual_cases.insert(item.AsString());
    }
    EXPECT_EQ(actual_cases, expected_cases);
    ASSERT_FALSE(cases->empty());
    EXPECT_EQ(cases->back().AsString(), "C-010");
  }
}

TEST(TargetConfigFilesTest, EdgeAndVsCodeDeclareDocumentedWorkarounds) {
  const auto load = [](std::string_view id) {
    return azookey::ipc::json::Parse(
        ReadFile(std::filesystem::path(AZOOKEY_COMPAT_TARGETS_DIR) / (std::string(id) + ".json")));
  };

  const auto edge = load("edge");
  ASSERT_TRUE(edge);
  const auto* edge_workarounds = edge->GetObject("workarounds");
  ASSERT_NE(edge_workarounds, nullptr);
  const azookey::ipc::json::Value edge_value(*edge_workarounds);
  EXPECT_EQ(edge_value.GetBool("prefer_get_text_ext"), true);
  EXPECT_EQ(edge_value.GetString("display_attribute_fallback"), "ime-default-rendering");
  const auto* edge_launch = edge->GetObject("launch");
  ASSERT_NE(edge_launch, nullptr);
  const azookey::ipc::json::Value edge_launch_value(*edge_launch);
  const auto* edge_document = edge_launch_value.GetObject("temporary_document");
  ASSERT_NE(edge_document, nullptr);
  EXPECT_EQ(azookey::ipc::json::Value(*edge_document).GetBool("save_before_close"), false);

  const auto vscode = load("vscode");
  ASSERT_TRUE(vscode);
  const auto* vscode_workarounds = vscode->GetObject("workarounds");
  ASSERT_NE(vscode_workarounds, nullptr);
  const azookey::ipc::json::Value vscode_value(*vscode_workarounds);
  EXPECT_EQ(vscode_value.GetBool("prefer_ime_candidates_during_preedit"), true);
  EXPECT_EQ(vscode_value.GetBool("ignore_electron_ime_event_order"), true);
}

TEST(TargetConfigFilesTest, NotepadAllowsOnlyOwnedTemporaryDocumentInReusedWindow) {
  const auto path = std::filesystem::path(AZOOKEY_COMPAT_TARGETS_DIR) / "notepad.json";
  const auto parsed = azookey::ipc::json::Parse(ReadFile(path));
  ASSERT_TRUE(parsed);
  const auto* workarounds = parsed->GetObject("workarounds");
  ASSERT_NE(workarounds, nullptr);
  const azookey::ipc::json::Value workaround_value(*workarounds);
  EXPECT_EQ(workaround_value.GetBool("require_new_window"), false);
  EXPECT_EQ(workaround_value.GetBool("use_temporary_document"), true);
  EXPECT_EQ(workaround_value.GetBool("allow_reused_window_for_temporary_document"), true);

  const auto config = LoadTargetConfig(path);
  ASSERT_TRUE(config);
  EXPECT_FALSE(config->require_new_window);
  EXPECT_TRUE(config->use_temporary_document);
  EXPECT_TRUE(config->allow_reused_window_for_temporary_document);
}

TEST(TargetConfigLoaderTest, LoadsTemporaryDocumentEditorIdentityAndCloseGracePeriod) {
  TemporaryDirectory temp;
  const auto path = temp.path() / "target.json";
  WriteFile(path, kTargetConfigJson);

  const auto config = LoadTargetConfig(path);
  ASSERT_TRUE(config);
  EXPECT_EQ(config->id, "test-target");
  EXPECT_EQ(config->editor_control_type, "edit");
  EXPECT_EQ(config->editor_name, L"compat editor");
  EXPECT_TRUE(config->use_temporary_document);
  EXPECT_TRUE(config->save_temporary_document_before_close);
  EXPECT_FALSE(config->require_new_window);
  EXPECT_TRUE(config->allow_reused_window_for_temporary_document);
  EXPECT_EQ(config->close_grace_period_ms, 5000u);

  const auto document = CreateTemporaryDocument(*config);
  ASSERT_FALSE(document.empty());
  EXPECT_EQ(document.extension(), L".html");
  EXPECT_EQ(ReadFile(document), "<textarea aria-label=\"compat editor\"></textarea>");
  std::error_code ec;
  std::filesystem::remove(document, ec);
  EXPECT_FALSE(ec);
}

TEST(TargetConfigLoaderTest, RejectsInvalidIdentifierControlTypeAndExtension) {
  std::string invalid_id(kTargetConfigJson);
  invalid_id.replace(invalid_id.find("test-target"), 11, "test_target");
  EXPECT_FALSE(LoadTargetConfigText(invalid_id));

  std::string invalid_control_type(kTargetConfigJson);
  invalid_control_type.replace(invalid_control_type.find("\"edit\""), 6, "\"button\"");
  EXPECT_FALSE(LoadTargetConfigText(invalid_control_type));

  std::string invalid_extension(kTargetConfigJson);
  invalid_extension.replace(invalid_extension.find(".html"), 5, "html");
  EXPECT_FALSE(LoadTargetConfigText(invalid_extension));
}

TEST(TargetConfigLoaderTest, ReusedWindowRequiresTemporaryDocument) {
  std::string reused_window_without_temporary_document(kTargetConfigJson);
  constexpr std::string_view kTemporaryDocumentKey = "\"temporary_document\"";
  const auto key_position = reused_window_without_temporary_document.find(kTemporaryDocumentKey);
  ASSERT_NE(key_position, std::string::npos);
  reused_window_without_temporary_document.replace(key_position, kTemporaryDocumentKey.size(),
                                                   "\"ignored_document\"");
  EXPECT_FALSE(LoadTargetConfigText(reused_window_without_temporary_document));
}

TEST(TargetConfigLoaderTest, ReusedWindowRequiresSaveBeforeClose) {
  std::string reused_window_without_safe_save(kTargetConfigJson);
  const auto contents_key = reused_window_without_safe_save.find("\"contents\"");
  ASSERT_NE(contents_key, std::string::npos);
  reused_window_without_safe_save.insert(contents_key, "\"save_before_close\": false, ");
  EXPECT_FALSE(LoadTargetConfigText(reused_window_without_safe_save));
}

TEST(TargetConfigLoaderTest, ReusedWindowConflictsWithNewWindowRequirement) {
  std::string conflicting_requirements(kTargetConfigJson);
  constexpr std::string_view kDisabledRequirement = "\"require_new_window\": false";
  const auto require_new_window = conflicting_requirements.find(kDisabledRequirement);
  ASSERT_NE(require_new_window, std::string::npos);
  conflicting_requirements.replace(require_new_window, kDisabledRequirement.size(),
                                   "\"require_new_window\": true");
  EXPECT_FALSE(LoadTargetConfigText(conflicting_requirements));
}

TEST(TargetConfigLoaderTest, ResolvesSearchPathAppPathsAndQuotedConfiguredPaths) {
  const auto notepad = ResolveExecutable(L"notepad.exe");
  EXPECT_EQ(notepad.source, ExecutableResolutionSource::SearchPath);
  EXPECT_TRUE(std::filesystem::exists(notepad.path));

  const auto edge = ResolveExecutable(L"msedge.exe");
  EXPECT_NE(edge.source, ExecutableResolutionSource::Unresolved);
  EXPECT_TRUE(std::filesystem::exists(edge.path));

  TemporaryDirectory temp;
  const auto configured_path = temp.path() / "quoted app.exe";
  WriteFile(configured_path, "");
  const auto quoted = std::filesystem::path(L"\"" + configured_path.wstring() + L"\"");
  const auto configured = ResolveExecutable(quoted);
  EXPECT_EQ(configured.source, ExecutableResolutionSource::ConfiguredPath);
  EXPECT_EQ(configured.path, configured_path);
}

TEST(CompatReportWriterTest, RejectsInvalidTargetId) {
  TemporaryDirectory temp;
  TargetConfig target;
  target.id = "vs_code";
  EXPECT_FALSE(WriteReports(temp.path(), target, {}));
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

TEST(CaseSupportTest, CandidateGeometryUsesPhysicalDpiScalingAndMonitorBounds) {
  const RECT caret{100, 100, 102, 120};
  const RECT candidate_at_150{190, 120, 310, 220};
  const RECT monitor{0, 0, 400, 300};
  EXPECT_TRUE(IsCandidateNearCaretAtDpi(candidate_at_150, caret, 144));
  EXPECT_TRUE(IsRectWithinBounds(candidate_at_150, monitor));
  EXPECT_FALSE(IsCandidateNearCaretAtDpi(candidate_at_150, caret, 96));
  EXPECT_FALSE(IsRectWithinBounds(RECT{-1, 0, 10, 10}, monitor));
}

TEST(ClipboardIsolationTest, RestoresClipboardWhenActionFails) {
  FakeClipboardAccess clipboard;
  const auto status = RunWithClipboardIsolation(clipboard, L"deterministic",
                                                [] { return ClipboardActionStatus::Failed; });
  EXPECT_EQ(status, ClipboardIsolationStatus::ActionFailed);
  EXPECT_TRUE(clipboard.restored);
  EXPECT_EQ(clipboard.replacement, L"deterministic");
  EXPECT_EQ(clipboard.calls, (std::vector<std::string>{"backup", "replace", "restore"}));
}

TEST(ClipboardIsolationTest, RestoresClipboardWhenActionThrows) {
  FakeClipboardAccess clipboard;
  const auto status = RunWithClipboardIsolation(clipboard, L"deterministic", [] {
    throw std::runtime_error("fixed test exception");
    return ClipboardActionStatus::Completed;
  });
  EXPECT_EQ(status, ClipboardIsolationStatus::ActionFailed);
  EXPECT_TRUE(clipboard.restored);
  EXPECT_EQ(clipboard.calls, (std::vector<std::string>{"backup", "replace", "restore"}));
}

TEST(ClipboardIsolationTest, ReportsBackupUnavailableWithoutMutatingClipboard) {
  FakeClipboardAccess clipboard;
  clipboard.backup_succeeds = false;
  EXPECT_EQ(RunWithClipboardIsolation(clipboard, L"deterministic",
                                      [] { return ClipboardActionStatus::Completed; }),
            ClipboardIsolationStatus::BackupUnavailable);
  EXPECT_EQ(clipboard.calls, (std::vector<std::string>{"backup"}));
}

TEST(ClipboardIsolationTest, RestoresAfterReplacementFailure) {
  FakeClipboardAccess clipboard;
  clipboard.replacement_succeeds = false;
  EXPECT_EQ(RunWithClipboardIsolation(clipboard, L"deterministic",
                                      [] { return ClipboardActionStatus::Completed; }),
            ClipboardIsolationStatus::ReplacementFailed);
  EXPECT_TRUE(clipboard.restored);
  EXPECT_EQ(clipboard.calls, (std::vector<std::string>{"backup", "replace", "restore"}));
}

TEST(ClipboardIsolationTest, PreservesFailingSkipActionClassification) {
  FakeClipboardAccess clipboard;
  EXPECT_EQ(RunWithClipboardIsolation(clipboard, L"deterministic",
                                      [] { return ClipboardActionStatus::FailingSkip; }),
            ClipboardIsolationStatus::ActionSkipped);
  EXPECT_TRUE(clipboard.restored);
}

TEST(ClipboardIsolationTest, ReportsRestoreFailure) {
  FakeClipboardAccess clipboard;
  clipboard.restore_succeeds = false;
  EXPECT_EQ(RunWithClipboardIsolation(clipboard, L"deterministic",
                                      [] { return ClipboardActionStatus::Completed; }),
            ClipboardIsolationStatus::RestoreFailed);
  EXPECT_EQ(clipboard.calls, (std::vector<std::string>{"backup", "replace", "restore"}));
}

TEST(WindowOwnershipTest, SelectsNewWindowOwnedByLaunchedProcess) {
  const std::array observations{
      TargetWindowObservation{
          .window_id = 1, .process_id = 42, .existed_before = false, .executable_matches = true},
  };

  const auto selection = SelectTargetSurface(observations, 42, true);

  EXPECT_EQ(selection.window_id, 1u);
  EXPECT_EQ(selection.ownership, TargetSurfaceOwnership::Window);
  EXPECT_EQ(selection.failure, TargetSurfaceFailure::None);
  EXPECT_TRUE(selection.window_process_is_launched_process);
}

TEST(WindowOwnershipTest, SelectsNewWindowCreatedByExistingTargetProcess) {
  const std::array observations{
      TargetWindowObservation{
          .window_id = 1, .process_id = 7, .existed_before = true, .executable_matches = true},
      TargetWindowObservation{
          .window_id = 2, .process_id = 7, .existed_before = false, .executable_matches = true},
  };

  const auto selection = SelectTargetSurface(observations, 42, true);

  EXPECT_EQ(selection.window_id, 2u);
  EXPECT_EQ(selection.ownership, TargetSurfaceOwnership::Window);
  EXPECT_EQ(selection.failure, TargetSurfaceFailure::None);
  EXPECT_FALSE(selection.window_process_is_launched_process);
}

TEST(WindowOwnershipTest, SelectsTemporaryDocumentInReusedWindow) {
  const std::array observations{
      TargetWindowObservation{.window_id = 1,
                              .process_id = 7,
                              .existed_before = true,
                              .executable_matches = true,
                              .temporary_document_selected = true},
  };

  const auto selection = SelectTargetSurface(observations, 42, true);

  EXPECT_EQ(selection.window_id, 1u);
  EXPECT_EQ(selection.ownership, TargetSurfaceOwnership::DocumentTab);
  EXPECT_EQ(selection.failure, TargetSurfaceFailure::None);
  EXPECT_FALSE(selection.window_process_is_launched_process);
}

TEST(WindowOwnershipTest, ReportsMissingWindowWhenTargetCreatesNoWindow) {
  const auto selection = SelectTargetSurface(std::span<const TargetWindowObservation>{}, 42, true);

  EXPECT_EQ(selection.ownership, TargetSurfaceOwnership::None);
  EXPECT_EQ(selection.failure, TargetSurfaceFailure::NewWindowNotFound);
  EXPECT_STREQ(TargetSurfaceFailureReason(selection.failure), "new-target-window-not-found");
}

TEST(WindowOwnershipTest, RejectsReusedWindowWithoutDocumentOwnership) {
  const std::array observations{
      TargetWindowObservation{
          .window_id = 1, .process_id = 7, .existed_before = true, .executable_matches = true},
  };

  const auto selection = SelectTargetSurface(observations, 42, true);

  EXPECT_EQ(selection.ownership, TargetSurfaceOwnership::None);
  EXPECT_EQ(selection.failure, TargetSurfaceFailure::DocumentOwnershipNotEstablished);
  EXPECT_STREQ(TargetSurfaceFailureReason(selection.failure),
               "target-document-ownership-not-established");
}

TEST(WindowOwnershipTest, ReportsMissingWindowWhenOnlyOtherExecutablesMatchWindowClass) {
  const std::array observations{
      TargetWindowObservation{
          .window_id = 1, .process_id = 7, .existed_before = true, .executable_matches = false},
  };

  const auto selection = SelectTargetSurface(observations, 42, true);

  EXPECT_EQ(selection.ownership, TargetSurfaceOwnership::None);
  EXPECT_EQ(selection.failure, TargetSurfaceFailure::NewWindowNotFound);
}

}  // namespace
}  // namespace azookey::compat_test
