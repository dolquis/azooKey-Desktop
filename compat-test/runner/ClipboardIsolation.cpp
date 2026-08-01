#include "runner/ClipboardIsolation.h"

#include <Ole2.h>
#include <Windows.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

namespace azookey::compat_test {
namespace {

bool OpenClipboardWithRetry() {
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (OpenClipboard(nullptr)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

std::uint64_t HashBytes(const void* data, std::size_t size) {
  constexpr std::uint64_t kOffsetBasis = 14695981039346656037ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffsetBasis;
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= kPrime;
  }
  return hash;
}

bool FingerprintGlobalMemory(HANDLE handle, std::size_t* byte_size, std::uint64_t* content_hash) {
  const SIZE_T size = GlobalSize(handle);
  if (size == 0) return false;
  const void* data = GlobalLock(handle);
  if (!data) return false;
  *byte_size = size;
  *content_hash = HashBytes(data, size);
  GlobalUnlock(handle);
  return true;
}

HANDLE DuplicateClipboardData(UINT format, HANDLE source) {
  return OleDuplicateData(source, static_cast<CLIPFORMAT>(format), 0);
}

void FreeClipboardData(UINT format, HANDLE data) {
  if (!data) return;
  switch (format) {
    case CF_BITMAP:
    case CF_DSPBITMAP:
    case CF_PALETTE:
      DeleteObject(data);
      break;
    case CF_ENHMETAFILE:
    case CF_DSPENHMETAFILE:
      DeleteEnhMetaFile(static_cast<HENHMETAFILE>(data));
      break;
    case CF_METAFILEPICT:
    case CF_DSPMETAFILEPICT:
      if (auto* metafile = static_cast<METAFILEPICT*>(GlobalLock(data))) {
        DeleteMetaFile(metafile->hMF);
        GlobalUnlock(data);
      }
      GlobalFree(data);
      break;
    default:
      GlobalFree(data);
      break;
  }
}

}  // namespace

ClipboardIsolationStatus RunWithClipboardIsolation(
    ClipboardAccess& clipboard, std::wstring_view deterministic_text,
    const std::function<ClipboardActionStatus()>& action) {
  if (!clipboard.Backup()) return ClipboardIsolationStatus::BackupUnavailable;
  if (!clipboard.ReplaceWithDeterministicText(deterministic_text)) {
    return clipboard.Restore() ? ClipboardIsolationStatus::ReplacementFailed
                               : ClipboardIsolationStatus::RestoreFailed;
  }
  ClipboardActionStatus action_status = ClipboardActionStatus::Failed;
  try {
    action_status = action();
  } catch (...) {
    action_status = ClipboardActionStatus::Failed;
  }
  if (!clipboard.Restore()) return ClipboardIsolationStatus::RestoreFailed;
  switch (action_status) {
    case ClipboardActionStatus::Completed:
      return ClipboardIsolationStatus::Completed;
    case ClipboardActionStatus::FailingSkip:
      return ClipboardIsolationStatus::ActionSkipped;
    case ClipboardActionStatus::Failed:
      return ClipboardIsolationStatus::ActionFailed;
  }
  return ClipboardIsolationStatus::ActionFailed;
}

SystemClipboardAccess::~SystemClipboardAccess() {
  if (backup_complete_ && !restored_) Restore();
  for (auto& entry : saved_) {
    FreeClipboardData(entry.format, entry.data);
    entry.data = nullptr;
  }
}

bool SystemClipboardAccess::Backup() {
  if (backup_complete_) return false;
  if (!OpenClipboardWithRetry()) return false;
  std::vector<SnapshotEntry> snapshot;
  bool success = true;
  SetLastError(ERROR_SUCCESS);
  for (UINT format = EnumClipboardFormats(0); format != 0; format = EnumClipboardFormats(format)) {
    const HANDLE source = GetClipboardData(format);
    const HANDLE duplicate = source ? DuplicateClipboardData(format, source) : nullptr;
    if (!duplicate) {
      success = false;
      break;
    }
    SnapshotEntry entry;
    entry.format = format;
    entry.data = duplicate;
    entry.has_fingerprint =
        FingerprintGlobalMemory(duplicate, &entry.byte_size, &entry.content_hash);
    snapshot.push_back(entry);
    SetLastError(ERROR_SUCCESS);
  }
  if (GetLastError() != ERROR_SUCCESS) success = false;
  CloseClipboard();
  if (!success) {
    for (const auto& entry : snapshot) FreeClipboardData(entry.format, entry.data);
    return false;
  }
  saved_ = std::move(snapshot);
  backup_complete_ = true;
  return true;
}

bool SystemClipboardAccess::ReplaceWithDeterministicText(std::wstring_view text) {
  if (!backup_complete_ || !OpenClipboardWithRetry()) return false;
  bool success = false;
  if (EmptyClipboard()) {
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (storage) {
      if (void* memory = GlobalLock(storage)) {
        std::memcpy(memory, text.data(), text.size() * sizeof(wchar_t));
        GlobalUnlock(storage);
        if (SetClipboardData(CF_UNICODETEXT, storage)) {
          storage = nullptr;
          success = true;
        }
      }
      if (storage) GlobalFree(storage);
    }
  }
  CloseClipboard();
  return success;
}

bool SystemClipboardAccess::Restore() {
  if (!backup_complete_ || restored_) return false;
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (!OpenClipboardWithRetry()) continue;
    bool success = EmptyClipboard() != FALSE;
    for (const auto& entry : saved_) {
      if (!success) break;
      HANDLE duplicate = DuplicateClipboardData(entry.format, entry.data);
      if (!duplicate || !SetClipboardData(entry.format, duplicate)) {
        FreeClipboardData(entry.format, duplicate);
        success = false;
        break;
      }
    }
    CloseClipboard();
    if (success && VerifyRestoredSnapshot()) {
      restored_ = true;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

bool SystemClipboardAccess::VerifyRestoredSnapshot() const {
  if (!OpenClipboardWithRetry()) return false;
  SetLastError(ERROR_SUCCESS);
  const int format_count = CountClipboardFormats();
  bool matches = saved_.empty() ? format_count == 0 && GetLastError() == ERROR_SUCCESS : true;
  for (const auto& entry : saved_) {
    const HANDLE current = GetClipboardData(entry.format);
    if (!current) {
      matches = false;
      break;
    }
    if (entry.has_fingerprint) {
      std::size_t byte_size = 0;
      std::uint64_t content_hash = 0;
      if (!FingerprintGlobalMemory(current, &byte_size, &content_hash) ||
          byte_size != entry.byte_size || content_hash != entry.content_hash) {
        matches = false;
        break;
      }
    } else {
      HANDLE duplicate = DuplicateClipboardData(entry.format, current);
      if (!duplicate) {
        matches = false;
        break;
      }
      FreeClipboardData(entry.format, duplicate);
    }
  }
  CloseClipboard();
  return matches;
}

std::optional<std::wstring> SystemClipboardAccess::ReadUnicodeText() const {
  if (!OpenClipboardWithRetry()) return std::nullopt;
  std::optional<std::wstring> result;
  if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
    if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
      result = text;
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return result;
}

}  // namespace azookey::compat_test
