#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "azookey/core/PlatformPaths.h"
#include "azookey/host/HostArgs.h"

namespace {

azookey::host::HostArgsParseResult Parse(std::initializer_list<const char*> values) {
  std::vector<std::string> argv;
  for (const char* value : values) argv.emplace_back(value);
  return azookey::host::ParseHostArgs(argv, {}, "environment-token");
}

}  // namespace

TEST(HostArgsTest, ParsesBackendAliasesAndExplicitValues) {
  auto cuda_alias = Parse({"--cuda"});
  ASSERT_TRUE(cuda_alias);
  EXPECT_EQ(cuda_alias.args.config.backend, azookey::host::BackendKind::Cuda);
  EXPECT_TRUE(cuda_alias.args.explicit_backend);

  auto cpu_alias = Parse({"--cpu"});
  ASSERT_TRUE(cpu_alias);
  EXPECT_EQ(cpu_alias.args.config.backend, azookey::host::BackendKind::Cpu);

  auto cuda_value = Parse({"--backend", "cuda"});
  ASSERT_TRUE(cuda_value);
  EXPECT_EQ(cuda_value.args.config.backend, azookey::host::BackendKind::Cuda);

  auto cpu_value = Parse({"--backend", "cpu"});
  ASSERT_TRUE(cpu_value);
  EXPECT_EQ(cpu_value.args.config.backend, azookey::host::BackendKind::Cpu);

  auto vulkan_value = Parse({"--backend", "vulkan"});
  ASSERT_TRUE(vulkan_value);
  EXPECT_EQ(vulkan_value.args.config.backend, azookey::host::BackendKind::Vulkan);
  EXPECT_TRUE(vulkan_value.args.explicit_backend);
}

TEST(HostArgsTest, RejectsUnsupportedBackend) {
  const auto parsed = Parse({"--backend", "directml"});
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error, "unsupported backend: directml");
  EXPECT_FALSE(parsed.args.explicit_backend);
}

TEST(HostArgsTest, PipeDoesNotConsumeFollowingOption) {
  const auto parsed = Parse({"--pipe", "--backend", "cuda"});
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed.args.pipe_mode);
  EXPECT_TRUE(parsed.args.pipe_name.empty());
  EXPECT_EQ(parsed.args.config.backend, azookey::host::BackendKind::Cuda);
}

TEST(HostArgsTest, PipeConsumesOrdinaryName) {
  const auto parsed = Parse({"--pipe", R"(\\.\pipe\azookey-test)"});
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed.args.pipe_mode);
  EXPECT_EQ(parsed.args.pipe_name, R"(\\.\pipe\azookey-test)");
}

TEST(HostArgsTest, ParsesPathAndHandshakeOptions) {
  const auto parsed = Parse({"--model", "model.gguf", "--learning", "learning.tsv", "--user-dict",
                             "user.json", "--mock-dict", "mock.tsv", "--pipe-name", "pipe-name",
                             "--handshake-token", "cli-token"});
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.args.config.model_path, "model.gguf");
  EXPECT_TRUE(parsed.args.explicit_model_path);
  EXPECT_EQ(parsed.args.explicit_learning_path, std::filesystem::path("learning.tsv"));
  EXPECT_EQ(parsed.args.explicit_user_dict_path, std::filesystem::path("user.json"));
  EXPECT_EQ(parsed.args.mock_dict_path, "mock.tsv");
  EXPECT_TRUE(parsed.args.pipe_mode);
  EXPECT_EQ(parsed.args.pipe_name, "pipe-name");
  EXPECT_EQ(parsed.args.handshake_token, "cli-token");
}

TEST(HostArgsTest, UsesDefaultHandshakeTokenWhenNotOverridden) {
  const auto parsed = Parse({"--stdio"});
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.args.handshake_token, "environment-token");
}

TEST(HostArgsTest, UserDictConsumesRemainingArguments) {
  const auto parsed = Parse({"--pipe-name", "pipe-name", "userdict", "add", "--reading", "a"});
  ASSERT_TRUE(parsed);
  ASSERT_TRUE(parsed.args.userdict_args.has_value());
  EXPECT_EQ(*parsed.args.userdict_args, (std::vector<std::string>{"add", "--reading", "a"}));
}

TEST(HostArgsTest, LookupConsumesRemainingArguments) {
  const auto parsed =
      Parse({"--learning", "learning.tsv", "lookup", "--mode", "prefix", "--query", "a"});
  ASSERT_TRUE(parsed);
  ASSERT_TRUE(parsed.args.lookup_args.has_value());
  EXPECT_EQ(*parsed.args.lookup_args,
            (std::vector<std::string>{"--mode", "prefix", "--query", "a"}));
}

TEST(HostArgsTest, RejectsMissingValuesAndUnknownArguments) {
  for (const char* option : {"--backend", "--model", "--learning", "--user-dict", "--mock-dict",
                             "--pipe-name", "--handshake-token"}) {
    const auto parsed = Parse({option});
    ASSERT_FALSE(parsed) << option;
    EXPECT_EQ(parsed.error, std::string("missing value for ") + option);
  }

  const auto unknown = Parse({"--unknown"});
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error, "unknown argument: --unknown");
}

TEST(HostArgsTest, KeepsNonAsciiExplicitPathsAsUtf8) {
  // UTF-8 bytes for 日本語/学習.tsv and 日本語/辞書.json, written as escapes so
  // the expectation does not depend on how this file's literals are encoded.
  constexpr const char* kLearning =
      "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e/\xe5\xad\xa6\xe7\xbf\x92.tsv";
  constexpr const char* kUserDict =
      "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e/\xe8\xbe\x9e\xe6\x9b\xb8.json";

  const auto parsed = Parse({"--learning", kLearning, "--user-dict", kUserDict});
  ASSERT_TRUE(parsed);
  ASSERT_TRUE(parsed.args.explicit_learning_path.has_value());
  ASSERT_TRUE(parsed.args.explicit_user_dict_path.has_value());
  EXPECT_EQ(azookey::core::PathToUtf8(*parsed.args.explicit_learning_path), kLearning);
  EXPECT_EQ(azookey::core::PathToUtf8(*parsed.args.explicit_user_dict_path), kUserDict);
}
