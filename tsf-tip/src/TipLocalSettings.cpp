#include "azookey/tsf/TipLocalSettings.h"

#include <array>
#include <chrono>
#include <string>
#include <system_error>

#include "azookey/logging/RuntimeLogger.h"

namespace azookey::tsf {
namespace {
// File operations stay on activation/the watcher, never a keystroke callback.
std::string ReadBounded(const std::filesystem::path& path) {
  constexpr size_t kLimit = 1024 * 1024;
  std::string contents(kLimit + 1, '\0');
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  DWORD count = 0;
  const BOOL read =
      ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &count, nullptr);
  CloseHandle(file);
  if (!read || count > kLimit) return {};
  contents.resize(count);
  return contents;
}

class DirectoryWatch final {
 public:
  ~DirectoryWatch() { Close(); }
  bool Open(const std::filesystem::path& path) {
    Close();
    auto ancestor = path.parent_path().parent_path();
    std::error_code error;
    while (!ancestor.empty() && !std::filesystem::is_directory(ancestor, error)) {
      const auto parent = ancestor.parent_path();
      if (parent == ancestor) break;
      ancestor = parent;
      error.clear();
    }
    relative_ = path.lexically_relative(ancestor).native();
    directory_ =
        CreateFileW(ancestor.c_str(), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (directory_ == INVALID_HANDLE_VALUE) return false;
    operation_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return operation_.hEvent && Arm();
  }
  HANDLE Event() const { return pending_ ? operation_.hEvent : nullptr; }
  bool Consume() {
    DWORD transferred = 0;
    const BOOL success = GetOverlappedResult(directory_, &operation_, &transferred, FALSE);
    pending_ = false;
    bool relevant = !success || transferred == 0;
    size_t offset = 0;
    while (!relevant && offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= transferred) {
      const auto* change =
          reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer_.data() + offset);
      const auto length = change->FileNameLength / sizeof(WCHAR);
      if (offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) + change->FileNameLength >
          transferred)
        break;
      if (length <= relative_.size() &&
          CompareStringOrdinal(change->FileName, static_cast<int>(length), relative_.data(),
                               static_cast<int>(length), TRUE) == CSTR_EQUAL &&
          (length == relative_.size() || relative_[length] == L'\\'))
        relevant = true;
      if (!change->NextEntryOffset) break;
      offset += change->NextEntryOffset;
    }
    Arm();  // Keep notifications armed while parsing a changed file.
    return relevant;
  }

 private:
  bool Arm() {
    ResetEvent(operation_.hEvent);
    pending_ =
        ReadDirectoryChangesW(directory_, buffer_.data(), static_cast<DWORD>(buffer_.size()), TRUE,
                              FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                  FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                              nullptr, &operation_, nullptr) != FALSE;
    return pending_;
  }
  void Close() {
    if (pending_) {
      CancelIoEx(directory_, &operation_);
      DWORD transferred = 0;
      GetOverlappedResult(directory_, &operation_, &transferred, TRUE);
    }
    if (directory_ != INVALID_HANDLE_VALUE) CloseHandle(directory_);
    if (operation_.hEvent) CloseHandle(operation_.hEvent);
    directory_ = INVALID_HANDLE_VALUE;
    operation_ = {};
    pending_ = false;
  }
  HANDLE directory_{INVALID_HANDLE_VALUE};
  OVERLAPPED operation_{};
  alignas(DWORD) std::array<BYTE, 16384> buffer_{};
  std::wstring relative_;
  bool pending_{false};
};
}  // namespace

TipLocalSettings::~TipLocalSettings() { Stop(); }

bool TipLocalSettings::Start(const std::filesystem::path& settings_path) noexcept {
  Stop();
  try {
    path_ = std::filesystem::absolute(settings_path).lexically_normal();
    Reload();
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_ || !ready_) {
      Stop();
      return false;
    }
    worker_ = std::thread([this] { Watch(); });
    WaitForSingleObject(ready_, INFINITE);
    return watch_started_.load();
  } catch (...) {
    Stop();
    return false;
  }
}

void TipLocalSettings::Stop() noexcept {
  if (stop_) SetEvent(stop_);
  if (worker_.joinable()) worker_.join();
  if (stop_) CloseHandle(stop_);
  if (ready_) CloseHandle(ready_);
  stop_ = nullptr;
  ready_ = nullptr;
  watch_started_.store(false);
  const std::lock_guard<std::mutex> lock(mutex_);
  settings_ = {};
}

core::BracketSettings TipLocalSettings::Snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return settings_;
}

void TipLocalSettings::Reload() noexcept {
  core::BracketSettings next;
  try {
    next = core::ParseBracketSettings(ReadBounded(path_));
    const auto default_path = path_.parent_path().parent_path() / L"bracket-pairs.tsv";
    auto custom =
        std::filesystem::path(std::u8string(next.pairs_path.begin(), next.pairs_path.end()));
    // Relative paths resolve against the azooKey data directory, not an app's cwd.
    table_path_ = (custom.empty()         ? default_path
                   : custom.is_absolute() ? custom
                                          : default_path.parent_path() / custom)
                      .lexically_normal();
    auto parsed = core::ParseBracketTable(ReadBounded(table_path_));
    if (!parsed.invalid_lines.empty()) {
      static logging::RuntimeLogger logger(logging::RuntimeLoggerOptionsFromEnvironment("tip"));
      logger.Log(logging::RuntimeLogLevel::Warn, "bracket_table_invalid_rows",
                 {{"count", static_cast<uint64_t>(parsed.invalid_lines.size())},
                  {"first_line", static_cast<uint64_t>(parsed.invalid_lines.front())}});
    }
    next.table = std::make_shared<const core::BracketTable>(std::move(parsed.table));
  } catch (...) {
    next = {};  // No contents/paths in diagnostics, no writes to user configuration.
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = std::move(next);
  }
  changed_.notify_all();
}

void TipLocalSettings::Watch() noexcept {
  try {
    DirectoryWatch config, table;
    if (!config.Open(path_)) {
      SetEvent(ready_);
      return;
    }
    Reload();
    auto watched_table = table_path_;
    table.Open(watched_table);
    Reload();  // Close the read/registration race for both files.
    watch_started_.store(true);
    SetEvent(ready_);
    for (;;) {
      const HANDLE handles[]{stop_, config.Event(), table.Event()};
      if (!handles[1]) break;
      const DWORD wait = WaitForMultipleObjects(handles[2] ? 3 : 2, handles, FALSE, INFINITE);
      if (wait != WAIT_OBJECT_0 + 1 && wait != WAIT_OBJECT_0 + 2) break;
      if (!(wait == WAIT_OBJECT_0 + 1 ? config.Consume() : table.Consume())) continue;
      Reload();
      if (watched_table != table_path_ || !table.Event()) {
        watched_table = table_path_;
        table.Open(watched_table);
        Reload();
      }
    }
  } catch (...) {
    SetEvent(ready_);
  }
}

#ifdef AZOOKEY_TSF_TESTING
void TipLocalSettings::SetForTest(const core::BracketSettings& settings) {
  const std::lock_guard<std::mutex> lock(mutex_);
  settings_ = settings;
}

bool TipLocalSettings::WaitForSnapshotForTest(
    const std::function<bool(const core::BracketSettings&)>& predicate) {
  std::unique_lock<std::mutex> lock(mutex_);
  return changed_.wait_for(lock, std::chrono::seconds(5), [&] { return predicate(settings_); });
}

bool TipLocalSettings::WaitForEnabledForTest(bool enabled) {
  return WaitForSnapshotForTest(
      [&](const auto& settings) { return settings.pairing.enabled == enabled; });
}
#endif

}  // namespace azookey::tsf
