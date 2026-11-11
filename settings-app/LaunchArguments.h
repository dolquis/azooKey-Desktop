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
  // --pane <name> (sideload-packaging-spec section 3.2 / 3.5): the pane to show. Empty when not
  // supplied; an unknown name, a missing value or a repeated option is ignored, not an error.
  std::wstring pane;
  bool parse_error = false;
};

// The pane names --pane accepts, which are also the navigation items' Tags.
bool IsKnownPaneName(std::wstring_view name) noexcept;

LaunchArguments ParseLaunchArguments(std::wstring_view raw_arguments) noexcept;

// Whether the "バージョン" pane shows the launch context (sideload-packaging-spec section 3.2):
// only when the launch supplied --langid or --profile. A value that was not supplied stays
// NotSupplied and reads "未指定", even when another option was a parse error.
bool HasLaunchContext(const LaunchArguments& arguments) noexcept;

}  // namespace azookey::settings
