#pragma once

#include <chrono>
#include <filesystem>
#include <string_view>

namespace azookey::tsf {

enum class TipSettingWriteStatus {
  Written,
  LockUnavailable,  // Another writer held settings.json past the timeout.
  ReadFailed,
  InvalidFile,  // Not a JSON object; left as it is for the settings app to quarantine.
  WriteFailed,
};

std::string_view TipSettingWriteStatusName(TipSettingWriteStatus status);

// Sets one top-level boolean of settings.json and keeps every other key as it
// is on disk, under the file lock the settings app and the Host take, with an
// atomic replace (docs/tsf-deep-integration-spec.md §4.5). A missing file is
// created with only that key.
TipSettingWriteStatus WriteTipBoolSetting(const std::filesystem::path& path, std::string_view key,
                                          bool value, std::chrono::milliseconds lock_timeout);

}  // namespace azookey::tsf
