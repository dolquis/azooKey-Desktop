#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

struct IDataObject;

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
  ActionFailed,
  RestoreFailed,
};

ClipboardIsolationStatus RunWithClipboardIsolation(ClipboardAccess& clipboard,
                                                   std::wstring_view deterministic_text,
                                                   const std::function<bool()>& action);

class SystemClipboardAccess final : public ClipboardAccess {
 public:
  ~SystemClipboardAccess() override;

  bool Backup() override;
  bool ReplaceWithDeterministicText(std::wstring_view text) override;
  bool Restore() override;
  std::optional<std::wstring> ReadUnicodeText() const;

 private:
  IDataObject* saved_{nullptr};
  bool backup_complete_{false};
  bool restored_{false};
};

}  // namespace azookey::compat_test
