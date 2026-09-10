#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>
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

// A temp root shared between runs lets a concurrent Debug / Release or
// second-checkout run delete this run's fixture files mid-test, so each run
// creates a root nobody else can name and removes only that one.
std::string RandomIdentifier() {
  std::random_device device;
  const std::uint64_t value =
      (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
  std::ostringstream stream;
  stream << std::hex << value;
  return stream.str();
}

std::filesystem::path CreateUniqueTempRoot() {
  const auto base = std::filesystem::temp_directory_path();
  for (int attempt = 0; attempt < 8; ++attempt) {
    const auto candidate = base / ("azookey_lookup_cli_" + RandomIdentifier());
    std::error_code error;
    // `create_directory` reports false for an existing directory, so a
    // collision retries instead of adopting somebody else's root.
    if (std::filesystem::create_directory(candidate, error) && !error) return candidate;
  }
  throw std::runtime_error("failed to create a unique temporary directory");
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

class LookupCliTest : public ::testing::Test {
 protected:
  void SetUp() override {
    paths.root = CreateUniqueTempRoot();
    paths.learning = paths.root / "learning.tsv";
    paths.user_dict = paths.root / "user_dict.json";
  }

  void TearDown() override {
    // Only this run's own root, and never throwing out of teardown.
    std::error_code error;
    std::filesystem::remove_all(paths.root, error);
  }

  TestPaths paths;
};

TEST_F(LookupCliTest, RejectsMissingAndInvalidArguments) {
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

TEST_F(LookupCliTest, FailsWhenUserDictionaryLockIsUnavailable) {
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
}

TEST_F(LookupCliTest, ExactReadingFindsUserDictionaryAndLearningEntries) {
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
}

TEST_F(LookupCliTest, ReadingPrefixFindsAllMatchingReadingsOnly) {
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
}

TEST_F(LookupCliTest, SurfaceFindsMatchesAcrossSources) {
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
}

TEST_F(LookupCliTest, NoMatchInPopulatedStoresReturnsSuccessfulEmptyJson) {
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
}

TEST_F(LookupCliTest, EmptyStoresReturnSuccessfulEmptyJson) {
  const auto options = Parse({"--mode", "prefix", "--query", "に"});
  ASSERT_TRUE(options);

  const auto result = RunLookup(*options, paths);
  EXPECT_EQ(result.exit_code, 0);
  ASSERT_EQ(result.output_lines.size(), 1u);
  const auto json = azookey::ipc::json::Parse(result.output_lines.front());
  ASSERT_TRUE(json);
  EXPECT_TRUE(json->GetBool("ok").value_or(false));
  EXPECT_EQ(json->GetUInt("count"), 0u);
}

TEST_F(LookupCliTest, TsvOutputUsesDocumentedColumns) {
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
}

TEST_F(LookupCliTest, MalformedUserDictionaryIsNotQuarantinedOrChanged) {
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
}

#ifdef _WIN32
TEST_F(LookupCliTest, ReadsStoresFromNonAsciiWindowsPaths) {
  // Nested inside the fixture's unique root, so the non-ASCII names are still
  // exercised without a path a concurrent run could also claim.
  TestPaths non_ascii;
  non_ascii.root = paths.root / L"azookey_非ASCII_検索";
  non_ascii.learning = non_ascii.root / L"学習.tsv";
  non_ascii.user_dict = non_ascii.root / L"ユーザー辞書.json";
  std::filesystem::create_directories(non_ascii.root);
  SeedStores(non_ascii);

  const auto options = Parse({"--mode", "exact", "--query", "にほん"});
  ASSERT_TRUE(options);
  const auto result = RunLookup(*options, non_ascii);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output_lines.size(), 2u);
}
#endif
