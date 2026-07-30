#include "azookey/logging/RuntimeLogger.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "azookey/core/Redaction.h"

namespace azookey::logging {

namespace {

constexpr std::string_view kRedacted = "***redacted***";

std::string EnvironmentValue(const char* name) {
#ifdef _WIN32
  char* buffer = nullptr;
  size_t size = 0;
  if (_dupenv_s(&buffer, &size, name) != 0 || !buffer) return {};
  std::string value(buffer);
  std::free(buffer);
  return value;
#else
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
#endif
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return static_cast<char>(ch - 'A' + 'a');
    return static_cast<char>(ch);
  });
  return value;
}

bool IsSensitiveField(std::string_view key) {
  const auto lower = LowerAscii(std::string(key));
  constexpr std::array<std::string_view, 11> names = {
      "reading",  "surface",      "candidate",      "candidate.text", "candidate_text", "prompt",
      "raw_keys", "window_title", "confirmed_text", "commit_text",    "input_text",
  };
  return std::find(names.begin(), names.end(), lower) != names.end();
}

bool IsReservedField(std::string_view key) {
  return key == "ts" || key == "component" || key == "level" || key == "event";
}

bool IsWindowTitleField(std::string_view key) {
  return LowerAscii(std::string(key)) == "window_title";
}

std::string EscapeJson(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (ch < 0x20) {
          escaped += "\\u00";
          escaped.push_back(hex[(ch >> 4) & 0x0f]);
          escaped.push_back(hex[ch & 0x0f]);
        } else {
          escaped.push_back(static_cast<char>(ch));
        }
    }
  }
  return escaped;
}

std::string LevelName(RuntimeLogLevel level) {
  switch (level) {
    case RuntimeLogLevel::Info:
      return "info";
    case RuntimeLogLevel::Warn:
      return "warn";
    case RuntimeLogLevel::Error:
      return "error";
  }
  return "info";
}

int LevelRank(RuntimeLogLevel level) {
  switch (level) {
    case RuntimeLogLevel::Info:
      return 0;
    case RuntimeLogLevel::Warn:
      return 1;
    case RuntimeLogLevel::Error:
      return 2;
  }
  return 0;
}

std::string TimestampNow() {
  const auto now = std::chrono::system_clock::now();
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  const std::time_t value = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &value);
#else
  gmtime_r(&value, &utc);
#endif
  std::ostringstream timestamp;
  timestamp << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
            << milliseconds.count() << 'Z';
  return timestamp.str();
}

std::string DateFromTimestamp(std::string_view timestamp) {
  if (timestamp.size() < 10) return "unknown";
  std::string date(timestamp.substr(0, 10));
  date.erase(std::remove(date.begin(), date.end(), '-'), date.end());
  return date;
}

std::filesystem::path DefaultLogsDirectory() {
#ifdef _WIN32
  const auto local_app_data = EnvironmentValue("LOCALAPPDATA");
  if (local_app_data.empty()) return {};
  return std::filesystem::path(local_app_data) / "azooKey" / "logs";
#else
  const auto xdg_data_home = EnvironmentValue("XDG_DATA_HOME");
  if (!xdg_data_home.empty()) return std::filesystem::path(xdg_data_home) / "azooKey" / "logs";
  const auto home = EnvironmentValue("HOME");
  if (home.empty()) return {};
  return std::filesystem::path(home) / ".local" / "share" / "azooKey" / "logs";
#endif
}

RuntimeLogLevel ParseLevel(std::string value) {
  value = LowerAscii(std::move(value));
  if (value == "warn") return RuntimeLogLevel::Warn;
  if (value == "error") return RuntimeLogLevel::Error;
  return RuntimeLogLevel::Info;
}

std::string SerializeFieldValue(const RuntimeLogField& field) {
  if (IsSensitiveField(field.key)) {
    return "\"" + EscapeJson(kRedacted) + "\"";
  }
  return std::visit(
      [](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return "\"" + EscapeJson(azookey::core::RedactFreeText(value)) + "\"";
        } else if constexpr (std::is_same_v<T, bool>) {
          return value ? "true" : "false";
        } else {
          std::ostringstream out;
          out << value;
          return out.str();
        }
      },
      field.value);
}

void RotateIfNeeded(const RuntimeLoggerOptions& options, const std::filesystem::path& path,
                    uintmax_t incoming_bytes) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) return;
  const auto current_size = std::filesystem::file_size(path, ec);
  if (ec || current_size + incoming_bytes <= options.max_file_bytes) return;

  if (options.max_generations == 0) {
    std::filesystem::remove(path, ec);
    return;
  }
  for (size_t generation = options.max_generations; generation > 0; --generation) {
    const auto source =
        generation == 1
            ? path
            : std::filesystem::path(path.string() + "." + std::to_string(generation - 1));
    const auto target = std::filesystem::path(path.string() + "." + std::to_string(generation));
    std::filesystem::remove(target, ec);
    ec.clear();
    if (std::filesystem::exists(source, ec) && !ec) {
      std::filesystem::rename(source, target, ec);
      if (ec) return;
    }
    ec.clear();
  }
}

void AppendLine(const RuntimeLoggerOptions& options, const std::filesystem::path& path,
                const std::string& line) {
#ifdef _WIN32
  std::wstring mutex_name = L"Local\\azooKey-runtime-log-";
  for (const unsigned char ch : options.component) {
    mutex_name.push_back((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                 (ch >= '0' && ch <= '9')
                             ? static_cast<wchar_t>(ch)
                             : L'_');
  }
  HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
  if (!mutex) return;
  const DWORD wait = WaitForSingleObject(mutex, 2000);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
    CloseHandle(mutex);
    return;
  }

  RotateIfNeeded(options, path, line.size());
  HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
  }
  ReleaseMutex(mutex);
  CloseHandle(mutex);
#else
  RotateIfNeeded(options, path, line.size());
  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output) return;
  output.write(line.data(), static_cast<std::streamsize>(line.size()));
#endif
}

}  // namespace

RuntimeLoggerOptions RuntimeLoggerOptionsFromEnvironment(
    std::string component, std::filesystem::path logs_directory) noexcept {
  RuntimeLoggerOptions options;
  try {
    options.component = std::move(component);
    options.enabled = EnvironmentValue("AZOOKEY_LOG") == "1";
    options.minimum_level = ParseLevel(EnvironmentValue("AZOOKEY_LOG_LEVEL"));
    options.logs_directory =
        logs_directory.empty() ? DefaultLogsDirectory() : std::move(logs_directory);
    if (options.logs_directory.empty() || options.component.empty()) options.enabled = false;
  } catch (...) {
    options.enabled = false;
  }
  return options;
}

std::string SerializeRuntimeLogRecord(const RuntimeLogRecord& record) {
  std::ostringstream out;
  out << "{\"ts\":\"" << EscapeJson(record.timestamp) << "\",\"component\":\""
      << EscapeJson(record.component) << "\",\"level\":\"" << LevelName(record.level)
      << "\",\"event\":\"" << EscapeJson(record.event) << '"';
  for (const auto& field : record.fields) {
    if (field.key.empty() || IsReservedField(field.key) || IsWindowTitleField(field.key)) continue;
    out << ",\"" << EscapeJson(field.key) << "\":" << SerializeFieldValue(field);
  }
  out << '}';
  return out.str();
}

RuntimeLogger::RuntimeLogger(RuntimeLoggerOptions options) : options_(std::move(options)) {}

void RuntimeLogger::Log(RuntimeLogLevel level, std::string_view event,
                        std::initializer_list<RuntimeLogField> fields) noexcept {
  if (!options_.enabled || event.empty() || LevelRank(level) < LevelRank(options_.minimum_level)) {
    return;
  }
  try {
    RuntimeLogRecord record;
    record.timestamp = TimestampNow();
    record.component = options_.component;
    record.level = level;
    record.event = std::string(event);
    record.fields.assign(fields.begin(), fields.end());
    auto line = SerializeRuntimeLogRecord(record);
    line.push_back('\n');

    std::lock_guard lock(mutex_);
    std::error_code ec;
    std::filesystem::create_directories(options_.logs_directory, ec);
    if (ec) return;
    const auto path = options_.logs_directory /
                      (options_.component + "-" + DateFromTimestamp(record.timestamp) + ".jsonl");
    AppendLine(options_, path, line);
  } catch (...) {
    // Runtime logging is best-effort and must never affect input or host availability.
  }
}

}  // namespace azookey::logging
