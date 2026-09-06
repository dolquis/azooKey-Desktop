#include "azookey/tsf/TipLocalSettings.h"

#include <array>
#include <chrono>
#include <string>
#include <system_error>

namespace azookey::tsf {

TipLocalSettings::~TipLocalSettings() { Stop(); }

bool TipLocalSettings::Start(const std::filesystem::path& settings_path) noexcept {
  Stop();
  try {
    path_ = std::filesystem::absolute(settings_path).lexically_normal();
    // Watch an existing ancestor so a later config directory/file creation is
    // noticed too. Do not create or write user configuration from the TIP.
    auto ancestor = path_.parent_path().parent_path();
    std::error_code ec;
    while (!ancestor.empty() && !std::filesystem::is_directory(ancestor, ec)) {
      const auto parent = ancestor.parent_path();
      if (parent == ancestor) break;
      ancestor = parent;
      ec.clear();
    }
    relative_path_ = path_.lexically_relative(ancestor).native();
    Reload();
    directory_ =
        CreateFileW(ancestor.c_str(), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (directory_ == INVALID_HANDLE_VALUE) return false;
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_ || !ready_) {
      Stop();
      return false;
    }
    worker_ = std::thread([this] { Watch(); });
    // Start returns only after the first watch is armed. Reload in Watch closes
    // the race between the initial read and notification registration.
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
  if (directory_ != INVALID_HANDLE_VALUE) CloseHandle(directory_);
  if (stop_) CloseHandle(stop_);
  if (ready_) CloseHandle(ready_);
  directory_ = INVALID_HANDLE_VALUE;
  stop_ = nullptr;
  ready_ = nullptr;
  watch_started_.store(false);
}

core::BracketSettings TipLocalSettings::Snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return settings_;
}

void TipLocalSettings::Reload() noexcept {
  core::BracketSettings next;
  try {
    // Bound reads even when the file grows while being read. JSON contains
    // unrelated settings too, so never log its contents or parser input.
    constexpr size_t kMaxSettingsBytes = 1024 * 1024;
    std::string contents(kMaxSettingsBytes + 1, '\0');
    const HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      DWORD count = 0;
      const BOOL read =
          ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &count, nullptr);
      CloseHandle(file);
      if (read && count <= kMaxSettingsBytes) {
        contents.resize(count);
        next = core::ParseBracketSettings(contents);
      }
    }
  } catch (...) {
    // Missing, invalid, or unreadable settings always use schema defaults.
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    settings_ = next;
  }
  changed_.notify_all();
}

void TipLocalSettings::Watch() noexcept {
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!event) {
    SetEvent(ready_);
    return;
  }
  alignas(DWORD) std::array<BYTE, 16384> buffer{};
  OVERLAPPED operation{};
  operation.hEvent = event;
  bool initial = true;
  for (;;) {
    ResetEvent(event);
    if (!ReadDirectoryChangesW(directory_, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE,
                               FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                   FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                               nullptr, &operation, nullptr)) {
      SetEvent(ready_);
      break;
    }
    if (initial) {
      watch_started_.store(true);
      Reload();
      initial = false;
      SetEvent(ready_);
    }
    const HANDLE handles[]{stop_, event};
    const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    DWORD transferred = 0;
    if (wait != WAIT_OBJECT_0 + 1) {
      CancelIoEx(directory_, &operation);
      GetOverlappedResult(directory_, &operation, &transferred, TRUE);
      break;
    }
    if (!GetOverlappedResult(directory_, &operation, &transferred, FALSE)) break;
    bool relevant = transferred == 0;
    size_t offset = 0;
    while (!relevant && offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= transferred) {
      const auto* change = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
      const size_t length = change->FileNameLength / sizeof(WCHAR);
      if (offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) + change->FileNameLength >
          transferred)
        break;
      if (length <= relative_path_.size() &&
          CompareStringOrdinal(change->FileName, static_cast<int>(length), relative_path_.data(),
                               static_cast<int>(length), TRUE) == CSTR_EQUAL &&
          (length == relative_path_.size() || relative_path_[length] == L'\\'))
        relevant = true;
      if (!change->NextEntryOffset) break;
      offset += change->NextEntryOffset;
    }
    // Re-read after relevant notifications, including buffer overflow (zero bytes).
    // ReadDirectoryChangesW retains subsequent changes on the directory handle
    // until the next request, including edits during this bounded read.
    if (relevant) Reload();
  }
  CloseHandle(event);
}

#ifdef AZOOKEY_TSF_TESTING
void TipLocalSettings::SetForTest(const core::BracketSettings& settings) {
  const std::lock_guard<std::mutex> lock(mutex_);
  settings_ = settings;
}

bool TipLocalSettings::WaitForEnabledForTest(bool enabled) {
  std::unique_lock<std::mutex> lock(mutex_);
  return changed_.wait_for(lock, std::chrono::seconds(5),
                           [&] { return settings_.pairing.enabled == enabled; });
}
#endif

}  // namespace azookey::tsf
