#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/Dispatcher.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/NewWordsCli.h"
#include "azookey/host/RequestScheduler.h"
#include "azookey/ipc/Json.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/learning/AutoWordStore.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/UserDictionary.h"

namespace {

using azookey::learning::AutoWordState;
using azookey::learning::AutoWordStore;

constexpr uint64_t kNow = 1'700'000'000;

std::filesystem::path TestDir(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

std::optional<azookey::host::NewWordsCliOptions> Parse(std::initializer_list<const char*> args,
                                                       std::string* error = nullptr) {
  std::vector<std::string> values(args.begin(), args.end());
  return azookey::host::ParseNewWordsCliArgs(values, error);
}

azookey::host::NewWordsCliRunOptions OfflineRunOptions(const std::filesystem::path& path) {
  azookey::host::NewWordsCliRunOptions options;
  options.auto_word_store_path = path;
  options.prefer_pipe = false;
  return options;
}

std::string UniquePipeName(const char* stem) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::string("\\\\.\\pipe\\") + stem + "-" + std::to_string(stamp);
}

// Two pending words, the second seen more recently.
void SeedStore(const std::filesystem::path& path) {
  AutoWordStore store(path);
  store.Observe("阿頭季", "あずき", kNow, 3, false);
  store.Observe("azooKey社", "あずきーしゃ", kNow + 100, 3, false);
  ASSERT_TRUE(store.Save());
}

}  // namespace

TEST(NewWordsCliTest, RejectsInvalidArguments) {
  std::string error;
  EXPECT_FALSE(Parse({}, &error));
  EXPECT_EQ(error, "missing newwords subcommand");
  EXPECT_FALSE(Parse({"delete"}, &error));
  EXPECT_FALSE(Parse({"confirm", "--reading", "あずき"}, &error));
  EXPECT_EQ(error, "confirm requires --reading and --surface");
  EXPECT_FALSE(Parse({"list", "--state", "all"}, &error));
  EXPECT_FALSE(Parse({"list", "--format", "xml"}, &error));
  // Flags belong to their own subcommand.
  EXPECT_FALSE(Parse({"list", "--offline"}, &error));
  EXPECT_FALSE(Parse({"reject", "--reading", "a", "--surface", "b", "--state", "pending"}, &error));

  const auto reject = Parse({"reject", "--reading", "あずき", "--surface", "阿頭季", "--offline"});
  ASSERT_TRUE(reject);
  EXPECT_EQ(reject->command, azookey::host::NewWordsCliCommand::Reject);
  EXPECT_TRUE(reject->offline);
}

TEST(NewWordsCliTest, ListReadsTheStoreMostRecentFirst) {
  const auto path = TestDir("azookey_newwords_cli_list") / "auto_words.tsv";
  SeedStore(path);

  auto result = azookey::host::RunNewWordsCli(*Parse({"list"}), OfflineRunOptions(path));
  EXPECT_EQ(result.exit_code, 0);
  ASSERT_EQ(result.output_lines.size(), 2u);
  const auto first = azookey::ipc::json::Parse(result.output_lines[0]);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->GetString("surface"), "azooKey社");
  EXPECT_EQ(first->GetString("state"), "pending");
  EXPECT_EQ(first->GetString("source"), "mining");

  result =
      azookey::host::RunNewWordsCli(*Parse({"list", "--format", "tsv"}), OfflineRunOptions(path));
  ASSERT_EQ(result.output_lines.size(), 2u);
  EXPECT_EQ(result.output_lines[1], "あずき\t阿頭季\tmining\tpending\t1\t1700000000");

  // An empty state still prints one line, so a caller can tell "none" from a crash.
  result = azookey::host::RunNewWordsCli(*Parse({"list", "--state", "confirmed"}),
                                         OfflineRunOptions(path));
  EXPECT_EQ(result.exit_code, 0);
  ASSERT_EQ(result.output_lines.size(), 1u);
  EXPECT_EQ(result.output_lines[0], R"({"ok":true,"op":"list"})");

  std::filesystem::remove_all(path.parent_path());
}

TEST(NewWordsCliTest, OfflineResolveEditsTheFile) {
  const auto path = TestDir("azookey_newwords_cli_offline") / "auto_words.tsv";
  SeedStore(path);
  const auto run = OfflineRunOptions(path);

  auto result = azookey::host::RunNewWordsCli(
      *Parse({"confirm", "--reading", "あずき", "--surface", "阿頭季", "--offline"}), run);
  EXPECT_EQ(result.exit_code, 0);
  auto json = azookey::ipc::json::Parse(result.output_lines.at(0));
  ASSERT_TRUE(json);
  EXPECT_TRUE(json->GetBool("ok").value_or(false));
  EXPECT_TRUE(json->GetBool("changed").value_or(false));
  EXPECT_EQ(json->GetString("via"), "file");

  // A repeat is a success that changes nothing.
  result = azookey::host::RunNewWordsCli(
      *Parse({"confirm", "--reading", "あずき", "--surface", "阿頭季", "--offline"}), run);
  EXPECT_EQ(result.exit_code, 0);
  json = azookey::ipc::json::Parse(result.output_lines.at(0));
  ASSERT_TRUE(json);
  EXPECT_FALSE(json->GetBool("changed").value_or(true));

  result = azookey::host::RunNewWordsCli(
      *Parse({"reject", "--reading", "しらない", "--surface", "知らない", "--offline"}), run);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.error, "not_found");

  AutoWordStore reloaded(path);
  ASSERT_TRUE(reloaded.Load());
  EXPECT_EQ(reloaded.LookupConfirmed("あずき").size(), 1u);
  EXPECT_EQ(reloaded.ListByState(AutoWordState::Pending).size(), 1u);

  std::filesystem::remove_all(path.parent_path());
}

TEST(NewWordsCliTest, ResolveWithoutARunningHostAsksForOffline) {
  const auto path = TestDir("azookey_newwords_cli_nohost") / "auto_words.tsv";
  SeedStore(path);
  azookey::host::NewWordsCliRunOptions run;
  run.auto_word_store_path = path;
  run.pipe_name = UniquePipeName("azookey-newwords-cli-missing-pipe");

  const auto result = azookey::host::RunNewWordsCli(
      *Parse({"confirm", "--reading", "あずき", "--surface", "阿頭季"}), run);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.error.find("--offline"), std::string::npos);
  // The file is left alone: a host that is merely slow would overwrite it.
  AutoWordStore reloaded(path);
  ASSERT_TRUE(reloaded.Load());
  EXPECT_TRUE(reloaded.LookupConfirmed("あずき").empty());

  std::filesystem::remove_all(path.parent_path());
}

TEST(NewWordsCliTest, ConfirmThroughTheRunningHostInjectsTheWord) {
  const auto dir = TestDir("azookey_newwords_cli_host");
  const auto path = dir / "auto_words.tsv";
  SeedStore(path);

  azookey::learning::LearningStore learning(dir / "learning.tsv");
  azookey::learning::UserDictionary user_dict(dir / "user_dict.json");
  AutoWordStore auto_words(path);
  ASSERT_TRUE(auto_words.Load());
  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(),
                                        &learning, {});
  engine.SetUserDictionary(&user_dict);
  engine.SetAutoWordStore(&auto_words);
  azookey::host::RequestScheduler scheduler;
  azookey::host::DispatcherConfig config;
  config.host_version = "newwords-cli-test-host";
  config.protocol_version = 1;
  config.handshake_token = "test-token";
  azookey::host::Dispatcher dispatcher(&engine, &scheduler, &user_dict, config, nullptr,
                                       &auto_words);

  const auto has_auto_word = [&]() {
    const auto candidates = engine.QueryCandidates("あずき", "", kNow + 200);
    return std::any_of(candidates.begin(), candidates.end(), [](const auto& c) {
      return c.surface == "阿頭季" && c.debug_info.find("auto-word") != std::string::npos;
    });
  };
  EXPECT_FALSE(has_auto_word());

  const std::string pipe_name = UniquePipeName("azookey-newwords-cli-test");
  std::mutex mutex;
  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name, [&](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        std::lock_guard<std::mutex> lock(mutex);
        return dispatcher.Dispatch(req);
      });
  if (!started) {
    GTEST_SKIP() << "NamedPipeTransport is Windows-only";
  }

  azookey::host::NewWordsCliRunOptions run;
  run.auto_word_store_path = path;
  run.pipe_name = pipe_name;
  run.handshake_token = "test-token";
  run.connect_timeout_ms = 2000;
  run.response_timeout_ms = 2000;

  auto result = azookey::host::RunNewWordsCli(
      *Parse({"confirm", "--reading", "あずき", "--surface", "阿頭季"}), run);
  EXPECT_EQ(result.exit_code, 0) << result.error;
  auto json = azookey::ipc::json::Parse(result.output_lines.at(0));
  ASSERT_TRUE(json);
  EXPECT_TRUE(json->GetBool("ok").value_or(false));
  EXPECT_TRUE(json->GetBool("changed").value_or(false));
  EXPECT_EQ(json->GetString("via"), "ipc");
  // pending -> confirmed in the running host, and the next conversion has it.
  EXPECT_TRUE(has_auto_word());

  // The host's error code reaches the caller.
  result = azookey::host::RunNewWordsCli(
      *Parse({"reject", "--reading", "しらない", "--surface", "知らない"}), run);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.error, "not_found");

  run.handshake_token = "wrong-token";
  result = azookey::host::RunNewWordsCli(
      *Parse({"reject", "--reading", "あずき", "--surface", "阿頭季"}), run);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.error, "handshake rejected by running host");

  server.Stop();
  engine.SetAutoWordStore(nullptr);
  std::filesystem::remove_all(dir);
}
