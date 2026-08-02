#include "azookey/logging/RuntimeLogger.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <locale>
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

#include "azookey/core/PlatformPaths.h"
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
  timestamp.imbue(std::locale::classic());
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
  const auto local_app_data = azookey::core::GetLocalAppDataDirectory();
  if (!local_app_data) return {};
  return *local_app_data / "azooKey" / "logs";
}

RuntimeLogLevel ParseLevel(std::string value) {
  value = LowerAscii(std::move(value));
  if (value == "warn") return RuntimeLogLevel::Warn;
  if (value == "error") return RuntimeLogLevel::Error;
  return RuntimeLogLevel::Info;
}

std::string SerializeFieldValue(const RuntimeLogField& field) {
  return std::visit(
      [&field](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, RuntimeLogSafeText>) {
          const std::string text = IsSensitiveRuntimeLogField(field.key)
                                       ? std::string(kRedacted)
                                       : azookey::core::RedactFreeText(value.value);
          return "\"" + EscapeJson(text) + "\"";
        } else if constexpr (std::is_same_v<T, bool>) {
          return value ? "true" : "false";
        } else {
          std::ostringstream out;
          out.imbue(std::locale::classic());
          out << value;
          return out.str();
        }
      },
      field.value);
}

bool IsLogFileForComponent(std::string_view filename, std::string_view component,
                           std::chrono::sys_days cutoff) {
  const std::string prefix = std::string(component) + "-";
  if (!filename.starts_with(prefix)) return false;

  const std::string_view suffix = filename.substr(prefix.size());
  constexpr std::string_view extension = ".jsonl";
  if (suffix.size() < 8 + extension.size() || suffix.substr(8, extension.size()) != extension) {
    return false;
  }
  const std::string_view generation = suffix.substr(8 + extension.size());
  if (!generation.empty() &&
      (generation.size() == 1 || generation.front() != '.' ||
       !std::all_of(generation.begin() + 1, generation.end(),
                    [](unsigned char ch) { return ch >= '0' && ch <= '9'; }))) {
    return false;
  }

  const std::string_view date = suffix.substr(0, 8);
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  const auto parse = [](std::string_view value, auto& output) {
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
  };
  if (!parse(date.substr(0, 4), year) || !parse(date.substr(4, 2), month) ||
      !parse(date.substr(6, 2), day)) {
    return false;
  }
  const std::chrono::year_month_day calendar_date{std::chrono::year(year),
                                                  std::chrono::month(month), std::chrono::day(day)};
  return calendar_date.ok() && std::chrono::sys_days(calendar_date) < cutoff;
}

void PruneExpiredLogs(const RuntimeLoggerOptions& options) {
  if (options.max_retention_days == 0) return;
  const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
  const auto cutoff = today - std::chrono::days(options.max_retention_days - 1);

  std::error_code ec;
  std::filesystem::directory_iterator entries(options.logs_directory, ec);
  if (ec) return;
  for (const auto& entry : entries) {
    if (!entry.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }
    const auto filename = entry.path().filename().string();
    if (!IsLogFileForComponent(filename, options.component, cutoff)) continue;
    std::filesystem::remove(entry.path(), ec);
    ec.clear();
  }
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
  class MutexGuard {
   public:
    explicit MutexGuard(HANDLE handle) : handle_(handle) {}
    ~MutexGuard() {
      if (acquired_) ReleaseMutex(handle_);
      if (handle_) CloseHandle(handle_);
    }
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

    void MarkAcquired() noexcept { acquired_ = true; }

   private:
    HANDLE handle_{nullptr};
    bool acquired_{false};
  };

  std::wstring mutex_name = L"Local\\azooKey-runtime-log-";
  for (const unsigned char ch : options.component) {
    mutex_name.push_back((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                 (ch >= '0' && ch <= '9')
                             ? static_cast<wchar_t>(ch)
                             : L'_');
  }
  HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
  if (!mutex) return;
  MutexGuard mutex_guard(mutex);
  const DWORD wait = WaitForSingleObject(mutex, options.mutex_wait_timeout_ms);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return;
  mutex_guard.MarkAcquired();

  RotateIfNeeded(options, path, line.size());
  HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
  }
#else
  RotateIfNeeded(options, path, line.size());
  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output) return;
  output.write(line.data(), static_cast<std::streamsize>(line.size()));
#endif
}

}  // namespace

bool IsSensitiveRuntimeLogField(std::string_view key) {
  const auto lower = LowerAscii(std::string(key));
  constexpr std::array<std::string_view, 10> fragments = {
      "reading", "surface", "candidate", "prompt", "raw_key",
      "title",   "text",    "preedit",   "query",  "composition",
  };
  return std::any_of(fragments.begin(), fragments.end(), [&lower](std::string_view fragment) {
    return lower.find(fragment) != std::string::npos;
  });
}

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
  out.imbue(std::locale::classic());
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

std::string RuntimeLogger::FormatRecord(
    RuntimeLogLevel level, std::string_view event,
    std::initializer_list<RuntimeLogField> fields) const noexcept {
  if (event.empty()) return {};
  try {
    RuntimeLogRecord record;
    record.timestamp = TimestampNow();
    record.component = options_.component;
    record.level = level;
    record.event = std::string(event);
    record.fields.assign(fields.begin(), fields.end());
    return SerializeRuntimeLogRecord(record);
  } catch (...) {
    return {};
  }
}

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
    if (!retention_checked_) {
      PruneExpiredLogs(options_);
      retention_checked_ = true;
    }
    const auto path = options_.logs_directory /
                      (options_.component + "-" + DateFromTimestamp(record.timestamp) + ".jsonl");
    AppendLine(options_, path, line);
  } catch (...) {
    // Runtime logging is best-effort and must never affect input or host availability.
  }
}

}  // namespace azookey::logging
