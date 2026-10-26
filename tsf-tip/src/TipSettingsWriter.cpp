#include "azookey/tsf/TipSettingsWriter.h"

#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include "azookey/ipc/Json.h"
#include "azookey/learning/AtomicFile.h"
#include "azookey/learning/FileLock.h"

namespace azookey::tsf {

namespace j = azookey::ipc::json;

std::string_view TipSettingWriteStatusName(TipSettingWriteStatus status) {
  switch (status) {
    case TipSettingWriteStatus::Written:
      return "written";
    case TipSettingWriteStatus::LockUnavailable:
      return "lock_unavailable";
    case TipSettingWriteStatus::ReadFailed:
      return "read_failed";
    case TipSettingWriteStatus::InvalidFile:
      return "invalid_file";
    case TipSettingWriteStatus::WriteFailed:
      break;
  }
  return "write_failed";
}

TipSettingWriteStatus WriteTipBoolSetting(const std::filesystem::path& path, std::string_view key,
                                          bool value, std::chrono::milliseconds lock_timeout) {
  const auto lock = learning::AcquireExclusiveFileLockForPath(path, lock_timeout);
  if (!lock) return TipSettingWriteStatus::LockUnavailable;

  j::Object root;
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  if (ec) return TipSettingWriteStatus::ReadFailed;
  if (exists) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return TipSettingWriteStatus::ReadFailed;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) return TipSettingWriteStatus::ReadFailed;
    auto parsed = j::Parse(buffer.str());
    if (!parsed || !parsed->IsObject()) return TipSettingWriteStatus::InvalidFile;
    root = std::get<j::Object>(std::move(parsed->data));
  }
  root[std::string(key)] = j::Value(value);
  // The same shape settings-app/SettingsDocument.cpp writes.
  std::string serialized = j::Stringify(j::Value(std::move(root)));
  serialized.push_back('\n');
  return learning::WriteTextFileAtomically(path, serialized) ? TipSettingWriteStatus::Written
                                                             : TipSettingWriteStatus::WriteFailed;
}

}  // namespace azookey::tsf
