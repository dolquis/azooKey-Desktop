#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::compat_test {

class ClipboardAccess {
 public:
  virtual ~ClipboardAccess() = default;
  virtual bool Backup() = 0;
  virtual bool ReplaceWithDeterministicText(std::wstring_view text) = 0;
  virtual bool Restore() = 0;
};

enum class ClipboardIsolationStatus {
  Completed,
  BackupUnavailable,
  ReplacementFailed,
  ActionSkipped,
  ActionFailed,
  RestoreFailed,
};

enum class ClipboardActionStatus {
  Completed,
  FailingSkip,
  Failed,
};

ClipboardIsolationStatus RunWithClipboardIsolation(
    ClipboardAccess& clipboard, std::wstring_view deterministic_text,
    const std::function<ClipboardActionStatus()>& action);

class SystemClipboardAccess final : public ClipboardAccess {
 public:
  ~SystemClipboardAccess() override;

  bool Backup() override;
  bool ReplaceWithDeterministicText(std::wstring_view text) override;
  bool Restore() override;
  std::optional<std::wstring> ReadUnicodeText() const;

 private:
  struct SnapshotEntry {
    unsigned int format{};
    void* data{};
    std::size_t byte_size{};
    std::uint64_t content_hash{};
    bool has_fingerprint{false};
  };

  bool VerifyRestoredSnapshot() const;

  std::vector<SnapshotEntry> saved_;
  bool backup_complete_{false};
  bool restored_{false};
};

}  // namespace azookey::compat_test
