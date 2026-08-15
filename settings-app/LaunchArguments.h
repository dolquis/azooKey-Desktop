#pragma once

#include <Windows.h>

#include <string_view>

namespace azookey::settings {

enum class LaunchArgumentStatus {
  NotSupplied,
  Valid,
  Invalid,
};

struct LaunchArguments {
  LaunchArgumentStatus langid_status = LaunchArgumentStatus::NotSupplied;
  LANGID langid = 0;
  LaunchArgumentStatus profile_status = LaunchArgumentStatus::NotSupplied;
  GUID profile{};
  bool parse_error = false;
};

LaunchArguments ParseLaunchArguments(std::wstring_view raw_arguments) noexcept;

}  // namespace azookey::settings
