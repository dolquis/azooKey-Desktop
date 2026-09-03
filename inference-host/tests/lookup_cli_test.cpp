#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "azookey/host/LookupCli.h"
#include "azookey/ipc/Json.h"
#include "azookey/learning/FileLock.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/UserDictionary.h"

namespace {

struct TestPaths {
  std::filesystem::path root;
  std::filesystem::path learning;
  std::filesystem::path user_dict;
};

TestPaths PreparePaths(const char* name) {
  TestPaths paths;
  paths.root = std::filesystem::temp_directory_path() / name;
  paths.learning = paths.root / "learning.tsv";
  paths.user_dict = paths.root / "user_dict.json";
  std::filesystem::remove_all(paths.root);
  std::filesystem::create_directories(paths.root);
  return paths;
}

void SeedStores(const TestPaths& paths) {
  azookey::learning::UserDictionary dictionary(paths.user_dict);
  dictionary.Add({"日本", "にほん", 1285, 501, -5.0});
  dictionary.Add({"日本語", "にほんご", std::nullopt, std::nullopt, -4.0});
  dictionary.Add({"食べる", "たべる", std::nullopt, std::nullopt, std::nullopt});
  ASSERT_TRUE(dictionary.Save());

  azookey::learning::LearningStore learning(paths.learning);
  learning.Observe("にほん", "二本", 1.5, 100);
  learning.Observe("にほんばし", "日本橋", 2.5, 200);
  learning.Observe("たべる", "食べる", 3.5, 300);
  ASSERT_TRUE(learning.Save());
}

std::optional<azookey::host::LookupCliOptions> Parse(std::initializer_list<const char*> args,
                                                     std::string* error = nullptr) {
  std::vector<std::string> values;
  for (const char* arg : args) values.emplace_back(arg);
  return azookey::host::ParseLookupCliArgs(values, error);
}

azookey::host::LookupCliResult RunLookup(const azookey::host::LookupCliOptions& options,
                                         const TestPaths& paths) {
  return azookey::host::RunLookupCli(options, {paths.learning, paths.user_dict});
}

}  // namespace

TEST(LookupCliTest, RejectsMissingAndInvalidArguments) {
  std::string error;
  EXPECT_FALSE(Parse({}, &error));
  EXPECT_EQ(error, "--mode is required");

  EXPECT_FALSE(Parse({"--mode", "invalid", "--query", "x"}, &error));
  EXPECT_EQ(error, "invalid --mode value: invalid");

  EXPECT_FALSE(Parse({"--mode", "exact"}, &error));
  EXPECT_EQ(error, "--query is required");

  EXPECT_FALSE(Parse({"--mode", "exact", "--query", ""}, &error));
  EXPECT_EQ(error, "--query must not be empty");

  EXPECT_FALSE(Parse({"--mode", "surface", "--query", "x", "--format", "xml"}, &error));
  EXPECT_EQ(error, "invalid --format value: xml");
}

TEST(LookupCliTest, FailsWhenUserDictionaryLockIsUnavailable) {
  const auto paths = PreparePaths("azookey_lookup_cli_lock_unavailable");
  const auto options = Parse({"--mode", "exact", "--query", "x"});
  ASSERT_TRUE(options);

  std::promise<bool> acquired;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::thread holder([&] {
    auto lock = azookey::learning::AcquireExclusiveFileLockForPath(paths.user_dict,
                                                                   std::chrono::milliseconds(1000));
    acquired.set_value(lock.has_value());
    if (lock) release_future.wait();
  });

  const bool has_lock = acquired.get_future().get();
  if (!has_lock) {
    holder.join();
    GTEST_SKIP() << "failed to acquire fixture lock";
  }

  azookey::host::LookupCliRunOptions run_options{paths.learning, paths.user_dict};
  run_options.user_dict_lock_timeout = std::chrono::milliseconds(0);
  const auto result = azookey::host::RunLookupCli(*options, run_options);

  release.set_value();
  holder.join();
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.error, "failed to lock user dictionary");
  std::filesystem::remove_all(paths.root);
}

TEST(LookupCliTest, ExactReadingFindsUserDictionaryAndLearningEntries) {
  const auto paths = PreparePaths("azookey_lookup_cli_exact");
  SeedStores(paths);
  const auto options = Parse({"--mode", "exact", "--query", "にほん"});
  ASSERT_TRUE(options);

  const auto result = RunLookup(*options, paths);
  EXPECT_EQ(result.exit_code, 0);
  ASSERT_EQ(result.output_lines.size(), 2u);
  bool saw_user_dict = false;
  bool saw_learning = false;
  for (const auto& line : result.output_lines) {
    const auto json = azookey::ipc::json::Parse(line);
    ASSERT_TRUE(json);
    EXPECT_EQ(json->GetString("mode"), "exact");
    EXPECT_EQ(json->GetString("query"), "にほん");
    EXPECT_EQ(json->GetString("reading"), "にほん");
    saw_user_dict |= json->GetString("source") == "user_dict";
    saw_learning |= json->GetString("source") == "learning";
  }
  EXPECT_TRUE(saw_user_dict);
  EXPECT_TRUE(saw_learning);
  std::filesystem::remove_all(paths.root);
}

TEST(LookupCliTest, ReadingPrefixFindsAllMatchingReadingsOnly) {
  const auto paths = PreparePaths("azookey_lookup_cli_prefix");
  SeedStores(paths);
  const auto options = Parse({"--mode", "prefix", "--query", "にほん"});
  ASSERT_TRUE(options);

  const auto result = RunLookup(*options, paths);
  EXPECT_EQ(result.exit_code, 0);
  ASSERT_EQ(result.output_lines.size(), 4u);
  for (const auto& line : result.output_lines) {
    const auto json = azookey::ipc::json::Parse(line);
    ASSERT_TRUE(json);
    const auto reading = json->GetString("reading");
    ASSERT_TRUE(reading);
    EXPECT_EQ(reading->rfind("にほん", 0), 0u);
  }
  std::filesystem::remove_all(paths.root);
}

TEST(LookupCliTest, SurfaceFindsMatchesAcrossSources) {
  const auto paths = PreparePaths("azookey_lookup_cli_surface");
  SeedStores(paths);
  const auto options = Parse({"--mode", "surface", "--query", "食べる"});
  ASSERT_TRUE(options);

  const auto result = RunLookup(*options, paths);
  EXPECT_EQ(result.exit_code, 0);
  ASSERT_EQ(result.output_lines.size(), 2u);
  const auto learning = azookey::ipc::json::Parse(result.output_lines[0]);
  const auto user_dict = azookey::ipc::json::Parse(result.output_lines[1]);
  ASSERT_TRUE(learning);
  ASSERT_TRUE(user_dict);
  EXPECT_EQ(learning->GetString("source"), "learning");
  EXPECT_EQ(user_dict->GetString("source"), "user_dict");
  EXPECT_EQ(learning->GetString("surface"), "食べる");
  EXPECT_EQ(user_dict->GetString("surface"), "食べる");
  std::filesystem::remove_all(paths.root);
}

TEST(LookupCliTest, NoMatchInPopulatedStoresReturnsSuccessfulEmptyJson) {
  const auto paths = PreparePaths("azookey_lookup_cli_no_match");
  SeedStores(paths);
  const auto options = Parse({"--mode", "exact", "--query", "missing"});
  ASSERT_TRUE(options);

  const auto result = RunLookup(*options, paths);
  EXPECT_EQ(result.exit_code, 0);
  ASSERT_EQ(result.output_lines.size(), 1u);
  const auto json = azookey::ipc::json::Parse(result.output_lines.front());
  ASSERT_TRUE(json);
  EXPECT_TRUE(json->GetBool("ok").value_or(false));
  EXPECT_EQ(json->GetUInt("count"), 0u);
  std::filesystem::remove_all(paths.root);
}

TEST(LookupCliTest, EmptyStoresReturnSuccessfulEmptyJson) {
  const auto paths = PreparePaths("azookey_lookup_cli_empty");
  const auto options = Parse({"--mode", "prefix", "--query", "に"});
  ASSERT_TRUE(options);

  const auto result = RunLookup(*options, paths);
  EXPECT_EQ(result.exit_code, 0);
  ASSERT_EQ(result.output_lines.size(), 1u);
  const auto json = azookey::ipc::json::Parse(result.output_lines.front());
  ASSERT_TRUE(json);
  EXPECT_TRUE(json->GetBool("ok").value_or(false));
  EXPECT_EQ(json->GetUInt("count"), 0u);
  std::filesystem::remove_all(paths.root);
}

TEST(LookupCliTest, TsvOutputUsesDocumentedColumns) {
  const auto paths = PreparePaths("azookey_lookup_cli_tsv");
  SeedStores(paths);
  const auto options = Parse({"--mode", "exact", "--query", "にほん", "--format", "tsv"});
  ASSERT_TRUE(options);

  const auto result = RunLookup(*options, paths);
  EXPECT_EQ(result.exit_code, 0);
  ASSERT_EQ(result.output_lines.size(), 2u);
  for (const auto& line : result.output_lines) {
    size_t tabs = 0;
    for (const char ch : line) tabs += ch == '\t' ? 1u : 0u;
    EXPECT_EQ(tabs, 6u);
  }
  std::filesystem::remove_all(paths.root);
}

TEST(LookupCliTest, MalformedUserDictionaryIsNotQuarantinedOrChanged) {
  const auto paths = PreparePaths("azookey_lookup_cli_read_only");
  {
    std::ofstream output(paths.user_dict, std::ios::binary);
    ASSERT_TRUE(output);
    output << "not valid json";
  }
  const auto options = Parse({"--mode", "surface", "--query", "x"});
  ASSERT_TRUE(options);

  const auto result = RunLookup(*options, paths);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.error, "failed to load user dictionary");
  EXPECT_TRUE(result.output_lines.empty());
  EXPECT_TRUE(std::filesystem::exists(paths.user_dict));
  std::ifstream input(paths.user_dict, std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()),
            "not valid json");
  input.close();
  size_t corrupt_files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(paths.root)) {
    corrupt_files +=
        entry.path().filename().string().find(".corrupt.") != std::string::npos ? 1u : 0u;
  }
  EXPECT_EQ(corrupt_files, 0u);
  std::filesystem::remove_all(paths.root);
}

#ifdef _WIN32
TEST(LookupCliTest, ReadsStoresFromNonAsciiWindowsPaths) {
  TestPaths paths;
  paths.root = std::filesystem::temp_directory_path() / L"azookey_非ASCII_検索";
  paths.learning = paths.root / L"学習.tsv";
  paths.user_dict = paths.root / L"ユーザー辞書.json";
  std::filesystem::remove_all(paths.root);
  std::filesystem::create_directories(paths.root);
  SeedStores(paths);

  const auto options = Parse({"--mode", "exact", "--query", "にほん"});
  ASSERT_TRUE(options);
  const auto result = RunLookup(*options, paths);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output_lines.size(), 2u);

  std::filesystem::remove_all(paths.root);
}
#endif
