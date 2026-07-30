#pragma once

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace azookey::logging {

enum class RuntimeLogLevel {
  Info,
  Warn,
  Error,
};

using RuntimeLogFieldValue = std::variant<int64_t, uint64_t, double, bool, std::string>;

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
};

RuntimeLoggerOptions RuntimeLoggerOptionsFromEnvironment(
    std::string component, std::filesystem::path logs_directory = {}) noexcept;

std::string SerializeRuntimeLogRecord(const RuntimeLogRecord& record);

class RuntimeLogger {
 public:
  explicit RuntimeLogger(RuntimeLoggerOptions options);

  bool enabled() const noexcept { return options_.enabled; }
  void Log(RuntimeLogLevel level, std::string_view event,
           std::initializer_list<RuntimeLogField> fields = {}) noexcept;

 private:
  RuntimeLoggerOptions options_;
  std::mutex mutex_;
};

}  // namespace azookey::logging
