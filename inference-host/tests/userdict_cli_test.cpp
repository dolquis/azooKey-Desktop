#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "azookey/core/PlatformPaths.h"
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
  options.user_dict_path = path;
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

  EXPECT_FALSE(Parse({"import"}, &error).has_value());
  EXPECT_EQ(error, "import path is required");

  EXPECT_FALSE(Parse({"export"}, &error).has_value());
  EXPECT_EQ(error, "export path is required");
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

#ifdef _WIN32
TEST(UserDictCliTest, DirectModePreservesNonAsciiWindowsPath) {
  const auto root = std::filesystem::temp_directory_path() / L"azookey_非ASCII_CLI";
  const auto path = root / L"ユーザー辞書.json";
  std::filesystem::remove_all(root);

  const auto add = Parse({"add", "--reading", "にほんご", "--surface", "日本語"});
  ASSERT_TRUE(add);
  const auto result = azookey::host::RunUserDictCli(*add, DirectRunOptions(path));
  ASSERT_EQ(result.exit_code, 0);
  EXPECT_TRUE(std::filesystem::exists(path));

  azookey::learning::UserDictionary loaded(path);
  ASSERT_TRUE(loaded.Load());
  ASSERT_EQ(loaded.Lookup("にほんご").size(), 1u);

  std::filesystem::remove_all(root);
}
#endif

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

TEST(UserDictCliTest, DirectImportExportRoundTrip) {
  const auto path = TestPath("azookey_userdict_cli_import_export");
  const auto root = path.parent_path();
  const auto import_path = root / "import.tsv";
  const auto export_path = root / "export.json";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  {
    std::ofstream tsv(import_path);
    ASSERT_TRUE(tsv.is_open());
    tsv << "nihongo\tNihongo\t1285\t501\t-5.5\n";
    tsv << "azookey\tazooKey\t\t\t\n";
    tsv << "bad\tBad\tnot-int\t\t\n";
    tsv << "\tNoReading\t\t\t\n";
    tsv << "nihongo\tNihongo\t1285\t501\t-10.0\n";
  }

  std::vector<std::string> import_args = {"import", import_path.string()};
  std::string error;
  auto import_options = azookey::host::ParseUserDictCliArgs(import_args, &error);
  ASSERT_TRUE(import_options.has_value()) << error;
  auto import_result = azookey::host::RunUserDictCli(*import_options, DirectRunOptions(path));
  EXPECT_EQ(import_result.exit_code, 0);
  ASSERT_EQ(import_result.output_lines.size(), 1u);
  auto import_json = azookey::ipc::json::Parse(import_result.output_lines.front());
  ASSERT_TRUE(import_json.has_value());
  EXPECT_EQ(import_json->GetString("op"), "import");
  EXPECT_TRUE(import_json->GetBool("ok").value_or(false));
  EXPECT_EQ(import_json->GetUInt("imported"), 3u);
  EXPECT_EQ(import_json->GetUInt("skipped"), 2u);

  azookey::learning::UserDictionary dict(path.string());
  ASSERT_TRUE(dict.Load());
  EXPECT_EQ(dict.Size(), 2u);
  auto matches = dict.Lookup("nihongo");
  ASSERT_EQ(matches.size(), 1u);
  ASSERT_TRUE(matches.front().value.has_value());
  EXPECT_DOUBLE_EQ(*matches.front().value, -10.0);

  std::vector<std::string> export_args = {"export", export_path.string()};
  auto export_options = azookey::host::ParseUserDictCliArgs(export_args, &error);
  ASSERT_TRUE(export_options.has_value()) << error;
  auto export_result = azookey::host::RunUserDictCli(*export_options, DirectRunOptions(path));
  EXPECT_EQ(export_result.exit_code, 0);
  ASSERT_EQ(export_result.output_lines.size(), 1u);
  auto export_json = azookey::ipc::json::Parse(export_result.output_lines.front());
  ASSERT_TRUE(export_json.has_value());
  EXPECT_EQ(export_json->GetString("op"), "export");
  EXPECT_TRUE(export_json->GetBool("ok").value_or(false));
  EXPECT_EQ(export_json->GetUInt("exported"), 2u);

  azookey::learning::UserDictionary exported(export_path.string());
  ASSERT_TRUE(exported.Load());
  EXPECT_EQ(exported.Size(), 2u);
  auto exported_matches = exported.Lookup("azookey");
  ASSERT_EQ(exported_matches.size(), 1u);
  EXPECT_EQ(exported_matches.front().word, "azooKey");

  std::filesystem::remove_all(root);
}

TEST(UserDictCliTest, DirectImportExportKeepsNonAsciiPathsAsUtf8) {
  // UTF-8 bytes for 日本語辞書, 取込.tsv and 書出.json, written as escapes so the
  // fixture pins the bytes the CLI receives from argv.
  constexpr const char* kDirName = "azookey_userdict_cli_\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
  constexpr const char* kImportName = "\xe5\x8f\x96\xe8\xbe\xbc.tsv";
  constexpr const char* kExportName = "\xe6\x9b\xb8\xe5\x87\xba.json";

  const auto root = std::filesystem::temp_directory_path() / azookey::core::Utf8Path(kDirName);
  const auto dict_path = root / azookey::core::Utf8Path("user_dict.json");
  const auto import_path = root / azookey::core::Utf8Path(kImportName);
  const auto export_path = root / azookey::core::Utf8Path(kExportName);
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  {
    std::ofstream tsv(import_path);
    ASSERT_TRUE(tsv.is_open());
    tsv << "\xe3\x81\xab\xe3\x81\xbb\xe3\x82\x93\xe3\x81\x94\t\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"
           "\t\t\t\n";
  }

  // The CLI receives UTF-8 argv, so the fixture must not go through
  // path::string(), which encodes with the active code page on Windows.
  std::vector<std::string> import_args = {"import", azookey::core::PathToUtf8(import_path)};
  std::string error;
  auto import_options = azookey::host::ParseUserDictCliArgs(import_args, &error);
  ASSERT_TRUE(import_options.has_value()) << error;
  auto import_result = azookey::host::RunUserDictCli(*import_options, DirectRunOptions(dict_path));
  ASSERT_EQ(import_result.exit_code, 0) << import_result.error;
  auto import_json = azookey::ipc::json::Parse(import_result.output_lines.front());
  ASSERT_TRUE(import_json.has_value());
  EXPECT_EQ(import_json->GetUInt("imported"), 1u);
  EXPECT_EQ(import_json->GetString("path"), azookey::core::PathToUtf8(import_path));

  std::vector<std::string> export_args = {"export", azookey::core::PathToUtf8(export_path)};
  auto export_options = azookey::host::ParseUserDictCliArgs(export_args, &error);
  ASSERT_TRUE(export_options.has_value()) << error;
  auto export_result = azookey::host::RunUserDictCli(*export_options, DirectRunOptions(dict_path));
  ASSERT_EQ(export_result.exit_code, 0) << export_result.error;
  EXPECT_TRUE(std::filesystem::exists(export_path));

  azookey::learning::UserDictionary exported(export_path);
  ASSERT_TRUE(exported.Load());
  auto matches = exported.Lookup("\xe3\x81\xab\xe3\x81\xbb\xe3\x82\x93\xe3\x81\x94");
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches.front().word, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");

  std::filesystem::remove_all(root);
}
