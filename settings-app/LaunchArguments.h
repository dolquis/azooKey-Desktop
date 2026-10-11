#pragma once

#include <Windows.h>

#include <string>
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
  // --proofread-session <token> (rich-features-spec X-3-6): the TIP's batch
  // correction session to open. Only a 32-digit lowercase hexadecimal token is
  // accepted; anything else is a parse error.
  LaunchArgumentStatus proofread_session_status = LaunchArgumentStatus::NotSupplied;
  std::string proofread_session_token;
  bool parse_error = false;
};

LaunchArguments ParseLaunchArguments(std::wstring_view raw_arguments) noexcept;

}  // namespace azookey::settings
