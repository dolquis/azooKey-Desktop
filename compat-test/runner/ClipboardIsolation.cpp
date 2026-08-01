#include "runner/ClipboardIsolation.h"

#include <Ole2.h>
#include <Windows.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace azookey::compat_test {
namespace {

bool OpenClipboardWithRetry() {
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (OpenClipboard(nullptr)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

}  // namespace

ClipboardIsolationStatus RunWithClipboardIsolation(ClipboardAccess& clipboard,
                                                   std::wstring_view deterministic_text,
                                                   const std::function<bool()>& action) {
  if (!clipboard.Backup()) return ClipboardIsolationStatus::BackupUnavailable;
  if (!clipboard.ReplaceWithDeterministicText(deterministic_text)) {
    return clipboard.Restore() ? ClipboardIsolationStatus::ReplacementFailed
                               : ClipboardIsolationStatus::RestoreFailed;
  }
  bool action_succeeded = false;
  try {
    action_succeeded = action();
  } catch (...) {
    action_succeeded = false;
  }
  if (!clipboard.Restore()) return ClipboardIsolationStatus::RestoreFailed;
  return action_succeeded ? ClipboardIsolationStatus::Completed
                          : ClipboardIsolationStatus::ActionFailed;
}

SystemClipboardAccess::~SystemClipboardAccess() {
  if (backup_complete_ && !restored_) Restore();
  if (saved_) saved_->Release();
}

bool SystemClipboardAccess::Backup() {
  if (backup_complete_) return false;
  if (OleGetClipboard(&saved_) != S_OK || !saved_) return false;
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
  if (!backup_complete_ || restored_ || !saved_) return false;
  HRESULT set_result = E_FAIL;
  for (int attempt = 0; attempt < 20 && FAILED(set_result); ++attempt) {
    set_result = OleSetClipboard(saved_);
    if (FAILED(set_result)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (FAILED(set_result)) return false;
  HRESULT flush_result = E_FAIL;
  for (int attempt = 0; attempt < 20 && FAILED(flush_result); ++attempt) {
    flush_result = OleFlushClipboard();
    if (FAILED(flush_result)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (FAILED(flush_result)) return false;
  restored_ = true;
  return true;
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
