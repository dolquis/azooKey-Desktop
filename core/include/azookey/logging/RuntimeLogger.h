#pragma once

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace azookey::logging {

enum class RuntimeLogLevel {
  Info,
  Warn,
  Error,
};

struct RuntimeLogSafeText {
  explicit RuntimeLogSafeText(std::string text) : value(std::move(text)) {}

  std::string value;
};

using RuntimeLogFieldValue = std::variant<int64_t, uint64_t, double, bool, RuntimeLogSafeText>;

struct RuntimeLogField {
  std::string key;
  RuntimeLogFieldValue value;
};

struct RuntimeLogRecord {
  std::string timestamp;
  std::string component;
  RuntimeLogLevel level{RuntimeLogLevel::Info};
  std::string event;
  std::vector<RuntimeLogField> fields;
};

struct RuntimeLoggerOptions {
  bool enabled{false};
  std::string component;
  std::filesystem::path logs_directory;
  RuntimeLogLevel minimum_level{RuntimeLogLevel::Info};
  uintmax_t max_file_bytes{5 * 1024 * 1024};
  size_t max_generations{3};
  size_t max_retention_days{7};
  uint32_t mutex_wait_timeout_ms{200};
};

RuntimeLoggerOptions RuntimeLoggerOptionsFromEnvironment(
    std::string component, std::filesystem::path logs_directory = {}) noexcept;

bool IsSensitiveRuntimeLogField(std::string_view key);
std::string SerializeRuntimeLogRecord(const RuntimeLogRecord& record);

class RuntimeLogger {
 public:
  explicit RuntimeLogger(RuntimeLoggerOptions options);

  bool enabled() const noexcept { return options_.enabled; }
  std::string FormatRecord(RuntimeLogLevel level, std::string_view event,
                           std::initializer_list<RuntimeLogField> fields = {}) const noexcept;
  void Log(RuntimeLogLevel level, std::string_view event,
           std::initializer_list<RuntimeLogField> fields = {}) noexcept;

 private:
  RuntimeLoggerOptions options_;
  std::mutex mutex_;
  bool retention_checked_{false};
};

}  // namespace azookey::logging
