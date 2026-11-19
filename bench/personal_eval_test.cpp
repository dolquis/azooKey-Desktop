#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "ConversionQuality.h"
#include "PersonalEval.h"
#include "azookey/ipc/Json.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/Sha256.h"

namespace azookey::bench {
namespace {

class ScopedDirectory {
 public:
  explicit ScopedDirectory(std::string_view name) {
    static std::atomic<uint64_t> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            (std::string(name) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }
  ~ScopedDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteBytes(const std::filesystem::path& path, std::string_view bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open());
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(output.good());
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::vector<std::string> DirectoryNames(const std::filesystem::path& directory) {
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

learning::LearningEntry Row(std::string reading, std::string surface, uint64_t commits,
                            uint64_t rejects, std::string app = {}) {
  learning::LearningEntry entry;
  entry.reading = std::move(reading);
  entry.surface = std::move(surface);
  entry.record.commit_count = commits;
  entry.record.reject_count = rejects;
  entry.record.weight = 0.8 * static_cast<double>(commits);
  entry.app_name = std::move(app);
  return entry;
}

PersonalPrivacy NoPrivacySettings() { return PersonalPrivacy{}; }

PersonalPrivacy PrivacyFrom(const std::string& settings_json) {
  ScopedDirectory directory("azookey-personal-privacy");
  const auto path = directory.path() / "settings.json";
  WriteBytes(path, settings_json);
  std::string error;
  auto privacy = LoadPersonalPrivacy(path, true, &error);
  EXPECT_TRUE(privacy) << error;
  return privacy ? *privacy : PersonalPrivacy{};
}

const PersonalEvaluationCase* FindCase(const PersonalEvaluationInput& input,
                                       const std::string& reading) {
  for (const auto& item : input.cases) {
    if (item.reading == reading) return &item;
  }
  return nullptr;
}

TEST(PersonalEvalTest, SumsAppRowsAndKeepsOnlyNetCommittedSurfacesAsAnswers) {
  // Spec section 15.3: rows that differ only in app_name are summed. A surface committed and then
  // undone with an immediate Backspace has one commit and one rejection, so it nets to zero and
  // is neither an answer nor a rejection.
  const std::vector<learning::LearningEntry> rows = {
      Row("きしゃ", "記者", 3, 0), Row("きしゃ", "記者", 1, 0, "notepad.exe"),
      Row("きしゃ", "汽車", 2, 0), Row("きしゃ", "貴社", 1, 1),
      Row("きしゃ", "帰社", 0, 2), Row("こうせい", "構成", 1, 1),
  };
  const auto input = BuildPersonalCases(rows, 0, NoPrivacySettings(), 0);
  ASSERT_EQ(input.cases.size(), 1u);
  const auto& item = input.cases.front();
  EXPECT_EQ(item.reading, "きしゃ");
  EXPECT_EQ(item.expected, "記者");
  EXPECT_EQ(item.acceptable, std::vector<std::string>{"汽車"});
  ASSERT_EQ(item.surfaces.size(), 4u);
  const auto counts = std::find_if(item.surfaces.begin(), item.surfaces.end(),
                                   [](const auto& surface) { return surface.surface == "記者"; });
  ASSERT_NE(counts, item.surfaces.end());
  EXPECT_EQ(counts->commit_count, 4u);
  EXPECT_EQ(input.skipped.no_net_commit, 1u);
  EXPECT_EQ(input.rows_read, rows.size());
}

TEST(PersonalEvalTest, BreaksExpectedTiesByCommitsThenSurfaceBytes) {
  const std::vector<learning::LearningEntry> rows = {
      // Net 2 each: 亜 has more commits.
      Row("あ", "阿", 2, 0),
      Row("あ", "亜", 3, 1),
      // Net 2 and 2 commits each: 井 (E4 BA 95) precedes 胃 (E8 83 83).
      Row("い", "胃", 2, 0),
      Row("い", "井", 2, 0),
  };
  const auto input = BuildPersonalCases(rows, 0, NoPrivacySettings(), 0);
  ASSERT_NE(FindCase(input, "あ"), nullptr);
  EXPECT_EQ(FindCase(input, "あ")->expected, "亜");
  EXPECT_EQ(FindCase(input, "あ")->acceptable, std::vector<std::string>{"阿"});
  ASSERT_NE(FindCase(input, "い"), nullptr);
  EXPECT_EQ(FindCase(input, "い")->expected, "井");
}

TEST(PersonalEvalTest, MaxCasesTakesTheMostCommittedReadingsDeterministically) {
  const std::vector<learning::LearningEntry> rows = {
      Row("う", "宇", 1, 0), Row("え", "絵", 5, 0), Row("い", "胃", 3, 0),
      Row("あ", "亜", 3, 0), Row("お", "尾", 1, 4),
  };
  const auto input = BuildPersonalCases(rows, 0, NoPrivacySettings(), 2);
  ASSERT_EQ(input.cases.size(), 2u);
  EXPECT_EQ(input.cases[0].reading, "え");
  // あ and い tie on 3; あ comes first in byte order.
  EXPECT_EQ(input.cases[1].reading, "あ");
  EXPECT_EQ(input.skipped.over_max_cases, 2u);
  EXPECT_EQ(input.skipped.no_net_commit, 1u);

  const auto all = BuildPersonalCases(rows, 0, NoPrivacySettings(), 0);
  ASSERT_EQ(all.cases.size(), 4u);
  EXPECT_EQ(all.cases[2].reading, "い");
  EXPECT_EQ(all.cases[3].reading, "う");
  EXPECT_EQ(all.skipped.over_max_cases, 0u);
}

TEST(PersonalEvalTest, ExcludesRowsOfSecureAndPrivateAppsButKeepsGlobalRows) {
  const auto privacy = PrivacyFrom(
      "{\"privacy\":{\"mode\":\"normal\",\"secureApps\":[\"Vault.exe\"]},"
      "\"profilesByApp\":{\"chat.exe\":{\"privacyMode\":\"private\"},"
      "\"bank.exe\":{\"privacyMode\":\"secure\"},"
      "\"diary.exe\":{\"learningEnabled\":false},"
      "\"default\":{\"privacyMode\":\"normal\"}}}");
  const std::vector<learning::LearningEntry> rows = {
      Row("かぎ", "鍵", 1, 0, "keepass.exe"),  // bundled secureApps
      Row("かぎ", "鍵", 1, 0, "VAULT.EXE"),    // user secureApps, case-insensitive
      Row("かぎ", "鍵", 1, 0, "chat.exe"),
      Row("かぎ", "鍵", 1, 0, "bank.exe"),
      Row("かぎ", "鍵", 1, 0, "diary.exe"),
      Row("かぎ", "鍵", 2, 0, "notepad.exe"),
      Row("かぎ", "鍵", 3, 0),  // global row
  };
  const auto input = BuildPersonalCases(rows, 0, privacy, 0);
  EXPECT_EQ(input.skipped.secure_app, 2u);
  EXPECT_EQ(input.skipped.app_privacy_mode, 2u);
  EXPECT_EQ(input.skipped.app_learning_disabled, 1u);
  ASSERT_EQ(input.cases.size(), 1u);
  ASSERT_EQ(input.cases.front().surfaces.size(), 1u);
  EXPECT_EQ(input.cases.front().surfaces.front().commit_count, 5u);
}

TEST(PersonalEvalTest, DefaultProfileExcludesAppRowsOnlyAndRowsCountOnce) {
  // Spec section 15.4: the default profile applies to rows with an app name; a global row stays.
  // A secure app with a private profile is counted once, as secure_app.
  const auto privacy = PrivacyFrom(
      "{\"profilesByApp\":{\"default\":{\"privacyMode\":\"private\"},"
      "\"keepass.exe\":{\"learningEnabled\":false}}}");
  const std::vector<learning::LearningEntry> rows = {
      Row("かぎ", "鍵", 1, 0, "keepass.exe"),
      Row("かぎ", "鍵", 1, 0, "notepad.exe"),
      Row("かぎ", "鍵", 4, 0),
  };
  const auto input = BuildPersonalCases(rows, 0, privacy, 0);
  EXPECT_EQ(input.skipped.secure_app, 1u);
  EXPECT_EQ(input.skipped.app_privacy_mode, 1u);
  EXPECT_EQ(input.skipped.app_learning_disabled, 0u);
  ASSERT_EQ(input.cases.size(), 1u);
  EXPECT_EQ(input.cases.front().surfaces.front().commit_count, 4u);
}

TEST(PersonalEvalTest, SettingsThatStopLearningStopTheRun) {
  ScopedDirectory directory("azookey-personal-stop");
  const auto path = directory.path() / "settings.json";
  for (const std::string settings :
       {"{\"privacy\":{\"mode\":\"secure\"}}", "{\"privacy\":{\"mode\":\"private\"}}",
        "{\"privacy\":{\"mode\":\"custom\",\"custom\":{\"learning\":false}}}",
        "{\"privacy\":{\"mode\":\"unknown\"}}", "{\"privacy\":\"normal\"}"}) {
    WriteBytes(path, settings);
    std::string error;
    EXPECT_FALSE(LoadPersonalPrivacy(path, true, &error)) << settings;
    EXPECT_FALSE(error.empty());
  }
  for (const std::string settings :
       {"{}", "{\"privacy\":{\"mode\":\"normal\"}}", "{\"privacy\":{\"mode\":\"offline\"}}",
        "{\"privacy\":{\"mode\":\"custom\",\"custom\":{\"learning\":true}}}"}) {
    WriteBytes(path, settings);
    std::string error;
    EXPECT_TRUE(LoadPersonalPrivacy(path, true, &error)) << settings << ": " << error;
  }
}

TEST(PersonalEvalTest, InvalidOrMissingSettingsStopTheRunWithoutMovingTheFile) {
  ScopedDirectory directory("azookey-personal-invalid");
  const auto path = directory.path() / "settings.json";
  const std::string broken = "{\"privacy\":";
  WriteBytes(path, broken);
  const auto before = DirectoryNames(directory.path());
  std::string error;
  EXPECT_FALSE(LoadPersonalPrivacy(path, true, &error));
  EXPECT_FALSE(LoadPersonalPrivacy(path, false, &error));
  // Unlike SettingsStore::Load, nothing is quarantined to settings.json.invalid*.
  EXPECT_EQ(DirectoryNames(directory.path()), before);
  EXPECT_EQ(ReadText(path), broken);

  // A missing file is the normal mode only at the default location.
  const auto missing = directory.path() / "absent.json";
  EXPECT_FALSE(LoadPersonalPrivacy(missing, true, &error));
  EXPECT_TRUE(LoadPersonalPrivacy(missing, false, &error));
  // A path that cannot be read as a file.
  std::filesystem::create_directories(directory.path() / "folder.json");
  EXPECT_FALSE(LoadPersonalPrivacy(directory.path() / "folder.json", true, &error));
}

TEST(PersonalEvalTest, CountsMalformedRowsOfTheStoreAndRowsWithInvalidText) {
  ScopedDirectory directory("azookey-personal-malformed");
  const auto legacy = directory.path() / "learning.tsv";
  // A plaintext v2 file: a read-only load neither encrypts nor rewrites it.
  const auto v2 = learning::LearningStoreV2PathFor(legacy);
  const std::string text = std::string(learning::kLearningStoreV2Header) +
                           "\nきしゃ\t記者\t0.8\t1999999000\t1\t0\t0\t\tcommit\t\nbroken line\n"
                           "\xff\xfe\t記者\t0.8\t1999999000\t1\t0\t0\t\tcommit\t\n";
  WriteBytes(v2, text);
  const auto before = DirectoryNames(directory.path());
  learning::LearningStore store(legacy);
  size_t malformed = 0;
  ASSERT_TRUE(store.LoadReadOnly(&malformed));
  const auto input = BuildPersonalCases(store.All(), malformed, NoPrivacySettings(), 0);
  // The broken line does not parse; the row whose reading is not UTF-8 cannot be evaluated.
  EXPECT_EQ(input.skipped.malformed_row, 2u);
  EXPECT_EQ(input.cases.size(), 1u);
  EXPECT_EQ(ReadText(v2), text);
  EXPECT_EQ(DirectoryNames(directory.path()), before);
}

TEST(PersonalEvalTest, DatasetHashFollowsTheCanonicalFramingAndIgnoresRowOrder) {
  std::vector<learning::LearningEntry> rows = {
      Row("きしゃ", "記者", 3, 0),
      Row("きしゃ", "貴社", 1, 1),
      Row("あめ", "雨", 2, 0),
      Row("きしゃ", "記者", 1, 0, "notepad.exe"),
  };
  const auto input = BuildPersonalCases(rows, 0, NoPrivacySettings(), 0);
  std::string canonical;
  for (const auto* part :
       {"あめ", "雨", "2", "0", "きしゃ", "記者", "4", "0", "きしゃ", "貴社", "1", "1"}) {
    canonical += part;
    canonical.push_back('\0');
  }
  EXPECT_EQ(input.dataset_sha256, learning::Sha256Hex(canonical));

  std::reverse(rows.begin(), rows.end());
  EXPECT_EQ(BuildPersonalCases(rows, 0, NoPrivacySettings(), 0).dataset_sha256,
            input.dataset_sha256);

  // A reading the limit drops is not part of the hash.
  const auto limited = BuildPersonalCases(rows, 0, NoPrivacySettings(), 1);
  rows.push_back(Row("あめ", "飴", 1, 0));
  EXPECT_EQ(BuildPersonalCases(rows, 0, NoPrivacySettings(), 1).dataset_sha256,
            limited.dataset_sha256);
  EXPECT_NE(BuildPersonalCases(rows, 0, NoPrivacySettings(), 0).dataset_sha256,
            input.dataset_sha256);
}

TEST(PersonalEvalTest, CategoriesFollowTheExpectedSurface) {
  EXPECT_EQ(PersonalCategory("すし", "すし"), "personal_identity");
  EXPECT_EQ(PersonalCategory("すし", "寿司"), "personal_kanji");
  EXPECT_EQ(PersonalCategory("ひとびと", "人々"), "personal_kanji");
  EXPECT_EQ(PersonalCategory("こーひー", "コーヒー"), "personal_katakana");
  EXPECT_EQ(PersonalCategory("あいでぃー", "ID"), "personal_ascii");
  EXPECT_EQ(PersonalCategory("すし", "🍣"), "personal_other");
  EXPECT_EQ(PersonalCategory("すしや", "すし屋さん"), "personal_kanji");
  EXPECT_EQ(PersonalCategory("けーき", "ｹｰｷ"), "personal_other");
}

TEST(PersonalEvalTest, ValidatesBenchProfilesWithoutQuotingThem) {
  std::string error;
  EXPECT_TRUE(ValidateBenchProfile("日本語の文章", &error));
  EXPECT_TRUE(ValidateBenchProfile(std::string(30, 'a'), &error));
  const std::string secret = "ひみつのぷろふぁいる";
  // U+EE00, U+EE03 and U+EEFF are Zenzai control markers.
  const std::vector<std::string> invalid = {"",
                                            std::string(31, 'a'),
                                            secret + "\xEE\xB8\x80",
                                            secret + "\xEE\xB8\x83",
                                            secret + "\xEE\xBB\xBF",
                                            secret + "\xff"};
  // U+EDFF and U+EF00 are outside the marker range.
  EXPECT_TRUE(ValidateBenchProfile("\xEE\xB7\xBF\xEE\xBC\x80", &error));
  for (const auto& profile : invalid) {
    error.clear();
    EXPECT_FALSE(ValidateBenchProfile(profile, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(error.find(secret), std::string::npos);
  }

  ScopedDirectory directory("azookey-personal-profile");
  const auto path = directory.path() / "profile.txt";
  for (const auto& [content, expected] :
       std::vector<std::pair<std::string, std::string>>{{secret + "\n", secret},
                                                        {secret + "\r\n", secret},
                                                        {secret, secret},
                                                        {"a\n\n", "a\n"}}) {
    WriteBytes(path, content);
    const auto read = ReadBenchProfileFile(path, &error);
    ASSERT_TRUE(read) << error;
    EXPECT_EQ(*read, expected);
  }
  for (const std::string content :
       {std::string("\xEF\xBB\xBF") + secret, std::string("\n"), secret + std::string(31, 'a')}) {
    WriteBytes(path, content);
    error.clear();
    EXPECT_FALSE(ReadBenchProfileFile(path, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(error.find(secret), std::string::npos);
  }
  EXPECT_FALSE(ReadBenchProfileFile(directory.path() / "absent.txt", &error));
}

#if defined(_WIN32)
// Answers each reading with a fixed list of candidates.
CandidateQuery ScriptedQuery(std::map<std::string, std::vector<std::string>> surfaces) {
  return [surfaces = std::move(surfaces)](const std::string& reading, const std::string&) {
    std::vector<core::Candidate> candidates;
    const auto found = surfaces.find(reading);
    if (found == surfaces.end()) return candidates;
    for (const auto& surface : found->second) {
      core::Candidate candidate;
      candidate.surface = surface;
      candidate.reading = reading;
      candidates.push_back(std::move(candidate));
    }
    return candidates;
  };
}

std::vector<learning::LearningEntry> MetricRows() {
  return {
      // Case 1: 記者 (net 4) first, 汽車 (net 2) at rank 3; 帰社 is net rejected, not first.
      Row("きしゃ", "記者", 4, 0),
      Row("きしゃ", "汽車", 2, 0),
      Row("きしゃ", "帰社", 0, 1),
      // Case 2: 雨 (net 2) missing; the net rejected 飴 comes first.
      Row("あめ", "雨", 2, 0),
      Row("あめ", "飴", 1, 3),
      // Case 3: コーヒー (net 1) second.
      Row("こーひー", "コーヒー", 1, 0),
  };
}

TEST(PersonalEvalTest, ReportsSectionFifteenMetricsAndNoPersonalText) {
  ScopedDirectory directory("azookey-personal-metrics");
  const auto input = BuildPersonalCases(MetricRows(), 1, NoPrivacySettings(), 0);
  ASSERT_EQ(input.cases.size(), 3u);
  ConversionQualityOptions options;
  options.output_path = directory.path() / "result.json";
  options.iterations = 1;
  options.category = "general";  // Ignored: a personal run never filters.
  const auto query = ScriptedQuery({{"きしゃ", {"記者", "貴社", "汽車"}},
                                    {"あめ", {"飴", "天"}},
                                    {"こーひー", {"珈琲", "コーヒー"}}});
  std::ostringstream diagnostics;
  std::string error;
  ASSERT_TRUE(RunPersonalEvaluation(options, input, query, diagnostics, &error)) << error;
  const auto text = ReadText(options.output_path);
  const auto report = ipc::json::Parse(text);
  ASSERT_TRUE(report && report->IsObject());
  const auto& config = *report->FindObject("config");
  EXPECT_EQ(config.at("eval_source").AsString(), "personal");
  EXPECT_EQ(config.at("profile_sha256").AsString(), "none");
  EXPECT_EQ(config.at("category_filter").AsString(), "all");
  EXPECT_EQ(config.at("learning_state").AsString(), "empty");
  EXPECT_EQ(config.at("eval_dataset_sha256").AsString(), input.dataset_sha256);
  const auto& summary = *report->FindObject("summary");
  EXPECT_DOUBLE_EQ(summary.at("top1_accuracy").AsNumber(), 1.0 / 3.0);
  EXPECT_DOUBLE_EQ(summary.at("top5_accuracy").AsNumber(), 2.0 / 3.0);
  // Net commits: 4 + 2 + 2 + 1 = 9; first candidates cover 4, the top five 4 + 2 + 1.
  EXPECT_DOUBLE_EQ(summary.at("commit_coverage_top1").AsNumber(), 4.0 / 9.0);
  EXPECT_DOUBLE_EQ(summary.at("commit_coverage_top5").AsNumber(), 7.0 / 9.0);
  EXPECT_DOUBLE_EQ(summary.at("rejected_top1_rate").AsNumber(), 0.5);
  EXPECT_EQ(summary.at("n_cases").AsNumber(), 3.0);
  EXPECT_EQ(summary.at("n_rejected_cases").AsNumber(), 2.0);
  EXPECT_EQ(summary.at("n_rows_read").AsNumber(), 6.0);
  const auto& skipped = summary.at("skipped").AsObject();
  EXPECT_EQ(skipped.at("malformed_row").AsNumber(), 1.0);
  for (const auto* key : {"no_net_commit", "secure_app", "app_privacy_mode",
                          "app_learning_disabled", "over_max_cases"}) {
    EXPECT_EQ(skipped.at(key).AsNumber(), 0.0) << key;
  }
  EXPECT_EQ(summary.find("typo_correction_top1_accuracy"), summary.end());
  const auto& categories = *report->FindObject("by_category");
  EXPECT_EQ(categories.at("personal_kanji").AsObject().at("n").AsNumber(), 2.0);
  EXPECT_EQ(categories.at("personal_katakana").AsObject().at("n").AsNumber(), 1.0);
  EXPECT_TRUE(report->FindObject("diff_vs_baseline")->empty());

  EXPECT_NE(diagnostics.str().find(kUnsaltedHashWarning), std::string::npos);
  for (const auto& row : MetricRows()) {
    for (const auto& field : {row.reading, row.surface}) {
      EXPECT_EQ(text.find(field), std::string::npos) << "output holds a personal string";
      EXPECT_EQ(diagnostics.str().find(field), std::string::npos)
          << "diagnostics hold a personal string";
    }
  }
}

TEST(PersonalEvalTest, ReportsZeroCasesAsZeroRatios) {
  ScopedDirectory directory("azookey-personal-empty");
  ConversionQualityOptions options;
  options.output_path = directory.path() / "result.json";
  options.iterations = 1;
  std::ostringstream diagnostics;
  std::string error;
  ASSERT_TRUE(RunPersonalEvaluation(options, BuildPersonalCases({}, 0, NoPrivacySettings(), 0),
                                    ScriptedQuery({}), diagnostics, &error))
      << error;
  const auto report = ipc::json::Parse(ReadText(options.output_path));
  ASSERT_TRUE(report && report->IsObject());
  const auto& summary = *report->FindObject("summary");
  EXPECT_EQ(summary.at("n_cases").AsNumber(), 0.0);
  for (const auto* key : {"top1_accuracy", "top5_accuracy", "MRR", "cer", "commit_coverage_top1",
                          "commit_coverage_top5", "rejected_top1_rate"}) {
    EXPECT_EQ(summary.at(key).AsNumber(), 0.0) << key;
  }
  EXPECT_NE(diagnostics.str().find(kUnsaltedHashWarning), std::string::npos);
}

TEST(PersonalEvalTest, WritesPerCaseOnlyWhenAllowedAndInCaseOrder) {
  ScopedDirectory directory("azookey-personal-per-case");
  const auto input = BuildPersonalCases(MetricRows(), 0, NoPrivacySettings(), 0);
  ConversionQualityOptions options;
  options.output_path = directory.path() / "result.json";
  options.per_case_path = directory.path() / "cases.jsonl";
  options.iterations = 1;
  const auto query = ScriptedQuery({{"きしゃ", {"記者"}}});
  std::ostringstream diagnostics;
  std::string error;
  EXPECT_FALSE(RunPersonalEvaluation(options, input, query, diagnostics, &error));
  EXPECT_FALSE(std::filesystem::exists(options.per_case_path));
  EXPECT_FALSE(std::filesystem::exists(options.output_path));

  options.include_personal_text = true;
  ASSERT_TRUE(RunPersonalEvaluation(options, input, query, diagnostics, &error)) << error;
  EXPECT_NE(diagnostics.str().find(kPersonalTextWarning), std::string::npos);
  std::istringstream lines(ReadText(options.per_case_path));
  std::string line;
  std::vector<std::string> ids;
  std::vector<std::string> inputs;
  while (std::getline(lines, line)) {
    const auto parsed = ipc::json::Parse(line);
    ASSERT_TRUE(parsed && parsed->IsObject());
    ids.push_back(parsed->AsObject().at("id").AsString());
    inputs.push_back(parsed->AsObject().at("input").AsString());
    EXPECT_EQ(parsed->AsObject().at("category").AsString().rfind("personal_", 0), 0u);
  }
  EXPECT_EQ(ids,
            (std::vector<std::string>{"personal_000001", "personal_000002", "personal_000003"}));
  std::vector<std::string> readings;
  for (const auto& item : input.cases) readings.push_back(item.reading);
  EXPECT_EQ(inputs, readings);
}

TEST(PersonalEvalTest, IncompatibleBaselineGivesNoDiffAndNamesTheKeys) {
  ScopedDirectory directory("azookey-personal-baseline");
  const auto input = BuildPersonalCases(MetricRows(), 0, NoPrivacySettings(), 0);
  ConversionQualityOptions options;
  options.output_path = directory.path() / "baseline.json";
  options.iterations = 1;
  const auto query = ScriptedQuery({{"きしゃ", {"記者"}}});
  std::ostringstream diagnostics;
  std::string error;
  ASSERT_TRUE(RunPersonalEvaluation(options, input, query, diagnostics, &error)) << error;

  options.baseline_path = options.output_path;
  options.output_path = directory.path() / "result.json";
  ASSERT_TRUE(RunPersonalEvaluation(options, input, query, diagnostics, &error)) << error;
  auto report = ipc::json::Parse(ReadText(options.output_path));
  ASSERT_TRUE(report && report->IsObject());
  EXPECT_DOUBLE_EQ(report->FindObject("diff_vs_baseline")->at("top1_accuracy").AsNumber(), 0.0);

  // Another profile and another NLL setting: both key names, no values, and no diff.
  options.profile_sha256 = std::string(64, 'b');
  options.nll = "on";
  diagnostics.str({});
  ASSERT_TRUE(RunPersonalEvaluation(options, input, query, diagnostics, &error)) << error;
  report = ipc::json::Parse(ReadText(options.output_path));
  ASSERT_TRUE(report && report->IsObject());
  EXPECT_TRUE(report->FindObject("diff_vs_baseline")->empty());
  EXPECT_NE(diagnostics.str().find("nll profile_sha256"), std::string::npos) << diagnostics.str();
  EXPECT_EQ(diagnostics.str().find(std::string(64, 'b')), std::string::npos);

  // A bundled result is never comparable with a personal one, nor a baseline from before the
  // eval_source key, which counts as bundled.
  std::string bundled = ReadText(directory.path() / "baseline.json");
  const std::string personal = "\"eval_source\":\"personal\",";
  const auto at = bundled.find(personal);
  ASSERT_NE(at, std::string::npos);
  bundled.erase(at, personal.size());
  WriteBytes(directory.path() / "bundled.json", bundled);
  options.baseline_path = directory.path() / "bundled.json";
  options.profile_sha256 = "none";
  options.nll = "off";
  diagnostics.str({});
  ASSERT_TRUE(RunPersonalEvaluation(options, input, query, diagnostics, &error)) << error;
  EXPECT_NE(diagnostics.str().find("eval_source"), std::string::npos);
}
#endif

}  // namespace
}  // namespace azookey::bench
