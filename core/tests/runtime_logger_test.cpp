#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <locale>
#include <optional>
#include <string>
#include <utility>

#include "azookey/core/PlatformPaths.h"
#include "azookey/logging/RuntimeLogger.h"

namespace {

using azookey::logging::RuntimeLogField;
using azookey::logging::RuntimeLogger;
using azookey::logging::RuntimeLoggerOptions;
using azookey::logging::RuntimeLogLevel;
using azookey::logging::RuntimeLogRecord;
using azookey::logging::RuntimeLogSafeText;

RuntimeLogSafeText SafeLogText(std::string value) { return RuntimeLogSafeText(std::move(value)); }

std::filesystem::path TestDirectory(const char* name) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

std::string ReadAllLogs(const std::filesystem::path& directory) {
  std::string result;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream input(entry.path(), std::ios::binary);
    result.append(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }
  return result;
}

class ScopedEnvironment {
 public:
  ScopedEnvironment(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
    char* current = nullptr;
    size_t length = 0;
    if (_dupenv_s(&current, &length, name) == 0 && current) {
      previous_ = current;
      std::free(current);
    }
#else
    const char* current = std::getenv(name);
    if (current) previous_ = current;
#endif
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
  }

  ~ScopedEnvironment() {
#ifdef _WIN32
    _putenv_s(name_.c_str(), previous_ ? previous_->c_str() : "");
#else
    if (previous_) {
      setenv(name_.c_str(), previous_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
#endif
  }

 private:
  std::string name_;
  std::optional<std::string> previous_;
};

class TestNumberPunctuation : public std::numpunct<char> {
 protected:
  char do_decimal_point() const override { return ','; }
  char do_thousands_sep() const override { return '_'; }
  std::string do_grouping() const override { return "\3"; }
};

class ScopedGlobalLocale {
 public:
  ScopedGlobalLocale() : previous_(std::locale()) {
    std::locale::global(std::locale(previous_, new TestNumberPunctuation()));
  }

  ~ScopedGlobalLocale() { std::locale::global(previous_); }

 private:
  std::locale previous_;
};

}  // namespace

TEST(RuntimeLoggerTest, SchemaSnapshotIsStableJsonLine) {
  RuntimeLogRecord record{
      "2026-07-30T12:34:56.789Z",
      "tip",
      RuntimeLogLevel::Warn,
      "ipc_stale_response",
      {RuntimeLogField{"request_id", uint64_t{42}}, RuntimeLogField{"result", SafeLogText("error")},
       RuntimeLogField{"retrying", true}},
  };

  EXPECT_EQ(
      azookey::logging::SerializeRuntimeLogRecord(record),
      R"({"ts":"2026-07-30T12:34:56.789Z","component":"tip","level":"warn","event":"ipc_stale_response","request_id":42,"result":"error","retrying":true})");
}

TEST(RuntimeLoggerTest, SensitiveBodiesAreRedacted) {
  RuntimeLogRecord record{
      "2026-07-30T12:34:56.789Z",
      "tip",
      RuntimeLogLevel::Info,
      "candidate_ready",
      {RuntimeLogField{"reading", SafeLogText("private-reading")},
       RuntimeLogField{"surface_text", SafeLogText("private-surface")},
       RuntimeLogField{"top_candidate", SafeLogText("private-candidate")},
       RuntimeLogField{"window_title", SafeLogText("private-window")},
       RuntimeLogField{"candidate_count", uint64_t{3}}},
  };

  const auto serialized = azookey::logging::SerializeRuntimeLogRecord(record);
  EXPECT_EQ(serialized.find("private-reading"), std::string::npos);
  EXPECT_EQ(serialized.find("private-surface"), std::string::npos);
  EXPECT_EQ(serialized.find("private-candidate"), std::string::npos);
  EXPECT_EQ(serialized.find("private-window"), std::string::npos);
  EXPECT_EQ(serialized.find("window_title"), std::string::npos);
  EXPECT_NE(serialized.find("***redacted***"), std::string::npos);
  EXPECT_NE(serialized.find("\"candidate_count\":3"), std::string::npos);
}

TEST(RuntimeLoggerTest, EmptyAndReservedFieldsAreSuppressed) {
  RuntimeLogRecord record{
      "2026-07-30T12:34:56.789Z",
      "tip",
      RuntimeLogLevel::Info,
      "field_filter",
      {RuntimeLogField{"", SafeLogText("empty")},
       RuntimeLogField{"event", SafeLogText("overridden")},
       RuntimeLogField{"component", SafeLogText("overridden")},
       RuntimeLogField{"detail", SafeLogText("retained")}},
  };

  const auto serialized = azookey::logging::SerializeRuntimeLogRecord(record);
  EXPECT_EQ(serialized.find("empty"), std::string::npos);
  EXPECT_EQ(serialized.find("overridden"), std::string::npos);
  EXPECT_NE(serialized.find("\"detail\":\"retained\""), std::string::npos);
}

TEST(RuntimeLoggerTest, SerializationUsesClassicLocale) {
  ScopedGlobalLocale locale;
  RuntimeLogRecord record{
      "2026-07-30T12:34:56.789Z",
      "host",
      RuntimeLogLevel::Info,
      "locale_probe",
      {RuntimeLogField{"count", uint64_t{1234}}, RuntimeLogField{"ratio", 1234.5}},
  };

  const auto serialized = azookey::logging::SerializeRuntimeLogRecord(record);
  EXPECT_NE(serialized.find("\"count\":1234"), std::string::npos);
  EXPECT_NE(serialized.find("\"ratio\":1234.5"), std::string::npos);
  EXPECT_EQ(serialized.find("1_234"), std::string::npos);
  EXPECT_EQ(serialized.find("1234,5"), std::string::npos);
}

TEST(RuntimeLoggerTest, DisabledLoggerDoesNotCreateAFile) {
  const auto directory = TestDirectory("azookey_runtime_log_disabled");
  RuntimeLoggerOptions options;
  options.component = "host";
  options.logs_directory = directory;
  options.enabled = false;
  RuntimeLogger logger(std::move(options));

  logger.Log(RuntimeLogLevel::Info, "host_started");

  EXPECT_TRUE(std::filesystem::is_empty(directory));
  std::filesystem::remove_all(directory);
}

TEST(RuntimeLoggerTest, EnvironmentOptInControlsEnablementAndLevel) {
  const auto directory = TestDirectory("azookey_runtime_log_environment");
  {
    ScopedEnvironment enabled("AZOOKEY_LOG", "1");
    ScopedEnvironment level("AZOOKEY_LOG_LEVEL", "warn");
    auto options = azookey::logging::RuntimeLoggerOptionsFromEnvironment("host", directory);
    EXPECT_TRUE(options.enabled);
    EXPECT_EQ(options.minimum_level, RuntimeLogLevel::Warn);
    RuntimeLogger logger(std::move(options));
    logger.Log(RuntimeLogLevel::Info, "filtered");
    EXPECT_TRUE(std::filesystem::is_empty(directory));
    logger.Log(RuntimeLogLevel::Warn, "enabled");
    EXPECT_FALSE(std::filesystem::is_empty(directory));
  }
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  {
    ScopedEnvironment disabled("AZOOKEY_LOG", "");
    auto options = azookey::logging::RuntimeLoggerOptionsFromEnvironment("host", directory);
    EXPECT_FALSE(options.enabled);
    RuntimeLogger logger(std::move(options));
    logger.Log(RuntimeLogLevel::Error, "disabled");
    EXPECT_TRUE(std::filesystem::is_empty(directory));
  }
  std::filesystem::remove_all(directory);
}

TEST(RuntimeLoggerTest, EnvironmentOptInRequiresComponentAndResolvableDirectory) {
  ScopedEnvironment enabled("AZOOKEY_LOG", "1");
  const auto directory = TestDirectory("azookey_runtime_log_invalid_options");
  auto missing_component = azookey::logging::RuntimeLoggerOptionsFromEnvironment("", directory);
  EXPECT_FALSE(missing_component.enabled);

  const auto local_app_data = azookey::core::GetLocalAppDataDirectory();
  if (local_app_data) {
    const auto defaults = azookey::logging::RuntimeLoggerOptionsFromEnvironment("host");
    EXPECT_TRUE(defaults.enabled);
    EXPECT_EQ(defaults.logs_directory, *local_app_data / "azooKey" / "logs");
  }
  std::filesystem::remove_all(directory);
}

TEST(RuntimeLoggerTest, UnwritableDestinationDoesNotThrow) {
  const auto directory = TestDirectory("azookey_runtime_log_unwritable");
  const auto not_a_directory = directory / "blocked";
  {
    std::ofstream file(not_a_directory);
    file << "file";
  }
  RuntimeLoggerOptions options;
  options.component = "host";
  options.logs_directory = not_a_directory;
  options.enabled = true;
  RuntimeLogger logger(std::move(options));

  EXPECT_NO_THROW(logger.Log(RuntimeLogLevel::Error, "write_failed"));

  std::filesystem::remove_all(directory);
}

TEST(RuntimeLoggerTest, RotationKeepsConfiguredGenerationLimit) {
  const auto directory = TestDirectory("azookey_runtime_log_rotation");
  RuntimeLoggerOptions options;
  options.component = "host";
  options.logs_directory = directory;
  options.enabled = true;
  options.max_file_bytes = 220;
  options.max_generations = 2;
  RuntimeLogger logger(std::move(options));

  for (uint64_t index = 0; index < 20; ++index) {
    logger.Log(RuntimeLogLevel::Info, "rotation_probe", {{"index", index}});
  }

  size_t file_count = 0;
  bool found_first_generation = false;
  bool found_second_generation = false;
  bool found_third_generation = false;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;
    ++file_count;
    if (entry.path().extension() == ".1") found_first_generation = true;
    if (entry.path().extension() == ".2") found_second_generation = true;
    if (entry.path().extension() == ".3") found_third_generation = true;
  }
  EXPECT_EQ(file_count, 3u);
  EXPECT_TRUE(found_first_generation);
  EXPECT_TRUE(found_second_generation);
  EXPECT_FALSE(found_third_generation);
  EXPECT_NE(ReadAllLogs(directory).find("\"event\":\"rotation_probe\""), std::string::npos);

  std::filesystem::remove_all(directory);
}

TEST(RuntimeLoggerTest, RetentionRemovesOnlyExpiredComponentLogs) {
  const auto directory = TestDirectory("azookey_runtime_log_retention");
  const auto old_host_log = directory / "host-20000101.jsonl";
  const auto old_host_generation = directory / "host-20000101.jsonl.1";
  const auto other_component_log = directory / "tip-20000101.jsonl";
  for (const auto& path : {old_host_log, old_host_generation, other_component_log}) {
    std::ofstream(path) << "old";
  }

  RuntimeLoggerOptions options;
  options.component = "host";
  options.logs_directory = directory;
  options.enabled = true;
  options.max_retention_days = 7;
  RuntimeLogger logger(std::move(options));
  logger.Log(RuntimeLogLevel::Info, "retention_probe");

  EXPECT_FALSE(std::filesystem::exists(old_host_log));
  EXPECT_FALSE(std::filesystem::exists(old_host_generation));
  EXPECT_TRUE(std::filesystem::exists(other_component_log));
  EXPECT_NE(ReadAllLogs(directory).find("\"event\":\"retention_probe\""), std::string::npos);

  std::filesystem::remove_all(directory);
}

TEST(RuntimeLoggerTest, FormatRecordPreservesFieldsAndRedactionForDebugView) {
  RuntimeLoggerOptions options;
  options.component = "tip";
  RuntimeLogger logger(std::move(options));

  const auto record = logger.FormatRecord(
      RuntimeLogLevel::Warn, "debug_probe",
      {{"request_id", uint64_t{42}}, {"preedit_text", SafeLogText("private-preedit")}});

  EXPECT_NE(record.find("\"event\":\"debug_probe\""), std::string::npos);
  EXPECT_NE(record.find("\"request_id\":42"), std::string::npos);
  EXPECT_EQ(record.find("private-preedit"), std::string::npos);
  EXPECT_NE(record.find("***redacted***"), std::string::npos);
}
