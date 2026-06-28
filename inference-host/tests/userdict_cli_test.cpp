#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "azookey/host/UserDictCli.h"
#include "azookey/ipc/Json.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/learning/UserDictionary.h"

namespace {

std::filesystem::path TestPath(const char* name) {
  return std::filesystem::temp_directory_path() / name / "user_dict.json";
}

azookey::host::UserDictCliRunOptions DirectRunOptions(const std::filesystem::path& path) {
  azookey::host::UserDictCliRunOptions options;
  options.user_dict_path = path.string();
  options.prefer_pipe = false;
  return options;
}

std::optional<azookey::host::UserDictCliOptions> Parse(std::initializer_list<const char*> args,
                                                       std::string* error = nullptr) {
  std::vector<std::string> values;
  for (const char* arg : args) {
    values.emplace_back(arg);
  }
  return azookey::host::ParseUserDictCliArgs(values, error);
}

std::string UniquePipeName(const char* stem) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::string("\\\\.\\pipe\\") + stem + "-" + std::to_string(stamp);
}

}  // namespace

TEST(UserDictCliTest, RejectsInvalidAddArguments) {
  std::string error;
  EXPECT_FALSE(Parse({"add", "--reading", "nihongo"}, &error).has_value());
  EXPECT_EQ(error, "--surface is required");

  EXPECT_FALSE(Parse({"add", "--reading", "nihongo", "--surface", "Nihongo", "--cid", "x"}, &error)
                   .has_value());
  EXPECT_EQ(error, "invalid --cid value: x");

  EXPECT_FALSE(
      Parse({"add", "--reading", "nihongo", "--surface", "Nihongo", "--weight", "inf"}, &error)
          .has_value());
  EXPECT_EQ(error, "invalid --weight value: inf");

  EXPECT_FALSE(Parse({"list", "--format", "xml"}, &error).has_value());
  EXPECT_EQ(error, "invalid --format value: xml");

  EXPECT_FALSE(Parse({"clear"}, &error).has_value());
  EXPECT_EQ(error, "unknown userdict subcommand: clear");
}

TEST(UserDictCliTest, DirectAddListRemoveRoundTrip) {
  const auto path = TestPath("azookey_userdict_cli_roundtrip");
  std::filesystem::remove_all(path.parent_path());

  auto add = Parse({"add", "--reading", "nihongo", "--surface", "Nihongo", "--cid", "1285", "--mid",
                    "501", "--weight", "-5.5"});
  ASSERT_TRUE(add.has_value());
  auto add_result = azookey::host::RunUserDictCli(*add, DirectRunOptions(path));
  EXPECT_EQ(add_result.exit_code, 0);
  ASSERT_EQ(add_result.output_lines.size(), 1u);
  auto add_json = azookey::ipc::json::Parse(add_result.output_lines.front());
  ASSERT_TRUE(add_json.has_value());
  EXPECT_TRUE(add_json->GetBool("ok").value_or(false));
  EXPECT_EQ(add_json->GetString("reading"), "nihongo");
  EXPECT_EQ(add_json->GetString("surface"), "Nihongo");

  azookey::learning::UserDictionary dict(path.string());
  ASSERT_TRUE(dict.Load());
  auto matches = dict.Lookup("nihongo");
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches.front().word, "Nihongo");

  auto list = Parse({"list"});
  ASSERT_TRUE(list.has_value());
  auto list_result = azookey::host::RunUserDictCli(*list, DirectRunOptions(path));
  EXPECT_EQ(list_result.exit_code, 0);
  ASSERT_EQ(list_result.output_lines.size(), 1u);
  auto list_json = azookey::ipc::json::Parse(list_result.output_lines.front());
  ASSERT_TRUE(list_json.has_value());
  EXPECT_EQ(list_json->GetString("op"), "list");
  EXPECT_EQ(list_json->GetString("reading"), "nihongo");
  EXPECT_EQ(list_json->GetString("surface"), "Nihongo");

  auto remove = Parse({"remove", "--reading", "nihongo", "--surface", "Nihongo"});
  ASSERT_TRUE(remove.has_value());
  auto remove_result = azookey::host::RunUserDictCli(*remove, DirectRunOptions(path));
  EXPECT_EQ(remove_result.exit_code, 0);
  ASSERT_EQ(remove_result.output_lines.size(), 1u);

  azookey::learning::UserDictionary after(path.string());
  ASSERT_TRUE(after.Load());
  EXPECT_TRUE(after.Lookup("nihongo").empty());

  std::filesystem::remove_all(path.parent_path());
}

TEST(UserDictCliTest, DirectDryRunDoesNotMutate) {
  const auto path = TestPath("azookey_userdict_cli_dryrun");
  std::filesystem::remove_all(path.parent_path());

  auto add = Parse({"add", "--reading", "dry", "--surface", "Run", "--dry-run"});
  ASSERT_TRUE(add.has_value());
  auto result = azookey::host::RunUserDictCli(*add, DirectRunOptions(path));
  EXPECT_EQ(result.exit_code, 0);
  ASSERT_EQ(result.output_lines.size(), 1u);
  auto json = azookey::ipc::json::Parse(result.output_lines.front());
  ASSERT_TRUE(json.has_value());
  EXPECT_TRUE(json->GetBool("dry_run").value_or(false));

  azookey::learning::UserDictionary dict(path.string());
  ASSERT_TRUE(dict.Load());
  EXPECT_EQ(dict.Size(), 0u);

  std::filesystem::remove_all(path.parent_path());
}

TEST(UserDictCliTest, DirectRemoveMissingReturnsNotFound) {
  const auto path = TestPath("azookey_userdict_cli_remove_missing");
  std::filesystem::remove_all(path.parent_path());

  auto remove = Parse({"remove", "--reading", "missing", "--surface", "Missing"});
  ASSERT_TRUE(remove.has_value());
  auto result = azookey::host::RunUserDictCli(*remove, DirectRunOptions(path));
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.error, "entry not found");
  ASSERT_EQ(result.output_lines.size(), 1u);
  auto json = azookey::ipc::json::Parse(result.output_lines.front());
  ASSERT_TRUE(json.has_value());
  EXPECT_FALSE(json->GetBool("ok").value_or(true));
  EXPECT_EQ(json->GetString("error"), "entry not found");

  std::filesystem::remove_all(path.parent_path());
}

TEST(UserDictCliTest, RequiresOfflineForDirectEditWhenPipeUnavailable) {
  const auto path = TestPath("azookey_userdict_cli_pipe_unavailable");
  std::filesystem::remove_all(path.parent_path());

  azookey::host::UserDictCliRunOptions run_options;
  run_options.user_dict_path = path.string();
  run_options.pipe_name = UniquePipeName("azookey-userdict-cli-missing-pipe");
  run_options.prefer_pipe = true;
  run_options.connect_timeout_ms = 1;
  run_options.response_timeout_ms = 100;

  auto add = Parse({"add", "--reading", "offline", "--surface", "Offline"});
  ASSERT_TRUE(add.has_value());
  auto result = azookey::host::RunUserDictCli(*add, run_options);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.error,
            "failed to connect to running host; pass --offline to edit the file directly");
  ASSERT_EQ(result.output_lines.size(), 1u);
  auto json = azookey::ipc::json::Parse(result.output_lines.front());
  ASSERT_TRUE(json.has_value());
  EXPECT_FALSE(json->GetBool("ok").value_or(true));
  EXPECT_EQ(json->GetString("via"), "ipc");
  EXPECT_EQ(json->GetString("error"),
            "failed to connect to running host; pass --offline to edit the file directly");
  EXPECT_FALSE(std::filesystem::exists(path));

  auto offline = Parse({"add", "--reading", "offline", "--surface", "Offline", "--offline"});
  ASSERT_TRUE(offline.has_value());
  auto offline_result = azookey::host::RunUserDictCli(*offline, run_options);
  EXPECT_EQ(offline_result.exit_code, 0);
  ASSERT_EQ(offline_result.output_lines.size(), 1u);
  auto offline_json = azookey::ipc::json::Parse(offline_result.output_lines.front());
  ASSERT_TRUE(offline_json.has_value());
  EXPECT_TRUE(offline_json->GetBool("ok").value_or(false));
  EXPECT_EQ(offline_json->GetString("via"), "direct");

  azookey::learning::UserDictionary dict(path.string());
  ASSERT_TRUE(dict.Load());
  auto matches = dict.Lookup("offline");
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches.front().word, "Offline");

  std::filesystem::remove_all(path.parent_path());
}

TEST(UserDictCliTest, PrefersRunningHostOverDirectEdit) {
  const auto path = TestPath("azookey_userdict_cli_ipc");
  std::filesystem::remove_all(path.parent_path());

  const std::string pipe_name = UniquePipeName("azookey-userdict-cli-test");
  std::mutex mutex;
  std::optional<azookey::ipc::AddUserWordRequest> add_seen;
  std::optional<azookey::ipc::RemoveUserWordRequest> remove_seen;

  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name, [&](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;

        if (req.type == azookey::ipc::MessageType::Handshake) {
          auto parsed = azookey::ipc::ParseHandshakeRequest(req.payload_json);
          azookey::ipc::HandshakeResponse payload;
          payload.host_version = "userdict-cli-test-host";
          payload.protocol_version = 1;
          payload.accepted =
              parsed && parsed->protocol_version == 1 && parsed->handshake_token == "test-token";
          payload.model_loaded = false;
          res.payload_json = azookey::ipc::BuildHandshakeResponse(payload);
          return res;
        }

        if (req.type == azookey::ipc::MessageType::AddUserWord) {
          auto parsed = azookey::ipc::ParseAddUserWordRequest(req.payload_json);
          {
            std::lock_guard<std::mutex> lock(mutex);
            add_seen = parsed;
          }
          azookey::ipc::AddUserWordResponse payload;
          payload.ok = parsed.has_value();
          res.payload_json = azookey::ipc::BuildAddUserWordResponse(payload);
          return res;
        }

        if (req.type == azookey::ipc::MessageType::RemoveUserWord) {
          auto parsed = azookey::ipc::ParseRemoveUserWordRequest(req.payload_json);
          {
            std::lock_guard<std::mutex> lock(mutex);
            remove_seen = parsed;
          }
          azookey::ipc::RemoveUserWordResponse payload;
          payload.ok = parsed.has_value();
          res.payload_json = azookey::ipc::BuildRemoveUserWordResponse(payload);
          return res;
        }

        return std::nullopt;
      });
  if (!started) {
    GTEST_SKIP() << "NamedPipeTransport is Windows-only";
  }

  azookey::host::UserDictCliRunOptions run_options;
  run_options.user_dict_path = path.string();
  run_options.pipe_name = pipe_name;
  run_options.handshake_token = "test-token";
  run_options.prefer_pipe = true;
  run_options.connect_timeout_ms = 2000;
  run_options.response_timeout_ms = 2000;

  auto add = Parse({"add", "--reading", "pipe", "--surface", "Pipe", "--cid", "100", "--mid", "200",
                    "--weight", "-1.25"});
  ASSERT_TRUE(add.has_value());
  auto add_result = azookey::host::RunUserDictCli(*add, run_options);
  EXPECT_EQ(add_result.exit_code, 0);
  ASSERT_EQ(add_result.output_lines.size(), 1u);
  auto add_json = azookey::ipc::json::Parse(add_result.output_lines.front());
  ASSERT_TRUE(add_json.has_value());
  EXPECT_TRUE(add_json->GetBool("ok").value_or(false));
  EXPECT_EQ(add_json->GetString("via"), "ipc");

  {
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_TRUE(add_seen.has_value());
    EXPECT_EQ(add_seen->ruby, "pipe");
    EXPECT_EQ(add_seen->word, "Pipe");
    EXPECT_EQ(add_seen->cid, 100);
    EXPECT_EQ(add_seen->mid, 200);
    ASSERT_TRUE(add_seen->value.has_value());
    EXPECT_DOUBLE_EQ(*add_seen->value, -1.25);
  }
  EXPECT_FALSE(std::filesystem::exists(path));

  auto remove = Parse({"remove", "--reading", "pipe", "--surface", "Pipe"});
  ASSERT_TRUE(remove.has_value());
  auto remove_result = azookey::host::RunUserDictCli(*remove, run_options);
  EXPECT_EQ(remove_result.exit_code, 0);
  ASSERT_EQ(remove_result.output_lines.size(), 1u);
  auto remove_json = azookey::ipc::json::Parse(remove_result.output_lines.front());
  ASSERT_TRUE(remove_json.has_value());
  EXPECT_TRUE(remove_json->GetBool("ok").value_or(false));
  EXPECT_EQ(remove_json->GetString("via"), "ipc");

  {
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_TRUE(remove_seen.has_value());
    EXPECT_EQ(remove_seen->ruby, "pipe");
    EXPECT_EQ(remove_seen->word, "Pipe");
  }
  EXPECT_FALSE(std::filesystem::exists(path));

  server.Stop();
  std::filesystem::remove_all(path.parent_path());
}

TEST(UserDictCliTest, RunningHostRejectionSurfacesFailure) {
  const auto path = TestPath("azookey_userdict_cli_ipc_rejected");
  std::filesystem::remove_all(path.parent_path());

  const std::string pipe_name = UniquePipeName("azookey-userdict-cli-reject-test");
  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name, [&](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;

        if (req.type == azookey::ipc::MessageType::Handshake) {
          azookey::ipc::HandshakeResponse payload;
          payload.host_version = "userdict-cli-test-host";
          payload.protocol_version = 1;
          payload.accepted = true;
          payload.model_loaded = false;
          res.payload_json = azookey::ipc::BuildHandshakeResponse(payload);
          return res;
        }

        if (req.type == azookey::ipc::MessageType::AddUserWord) {
          azookey::ipc::AddUserWordResponse payload;
          payload.ok = false;
          res.payload_json = azookey::ipc::BuildAddUserWordResponse(payload);
          return res;
        }

        return std::nullopt;
      });
  if (!started) {
    GTEST_SKIP() << "NamedPipeTransport is Windows-only";
  }

  azookey::host::UserDictCliRunOptions run_options;
  run_options.user_dict_path = path.string();
  run_options.pipe_name = pipe_name;
  run_options.prefer_pipe = true;
  run_options.connect_timeout_ms = 2000;
  run_options.response_timeout_ms = 2000;

  auto add = Parse({"add", "--reading", "reject", "--surface", "Reject"});
  ASSERT_TRUE(add.has_value());
  auto result = azookey::host::RunUserDictCli(*add, run_options);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.error, "running host rejected add");
  ASSERT_EQ(result.output_lines.size(), 1u);
  auto json = azookey::ipc::json::Parse(result.output_lines.front());
  ASSERT_TRUE(json.has_value());
  EXPECT_FALSE(json->GetBool("ok").value_or(true));
  EXPECT_EQ(json->GetString("via"), "ipc");
  EXPECT_EQ(json->GetString("error"), "running host rejected add");
  EXPECT_FALSE(std::filesystem::exists(path));

  server.Stop();
  std::filesystem::remove_all(path.parent_path());
}

TEST(UserDictCliTest, ListTsvUsesStableColumns) {
  const auto path = TestPath("azookey_userdict_cli_tsv");
  std::filesystem::remove_all(path.parent_path());

  auto add = Parse({"add", "--reading", "a", "--surface", "A"});
  ASSERT_TRUE(add.has_value());
  EXPECT_EQ(azookey::host::RunUserDictCli(*add, DirectRunOptions(path)).exit_code, 0);

  auto list = Parse({"list", "--format", "tsv"});
  ASSERT_TRUE(list.has_value());
  auto result = azookey::host::RunUserDictCli(*list, DirectRunOptions(path));
  EXPECT_EQ(result.exit_code, 0);
  ASSERT_EQ(result.output_lines.size(), 1u);
  EXPECT_EQ(result.output_lines.front(), "a\tA\t\t\t");

  std::filesystem::remove_all(path.parent_path());
}
