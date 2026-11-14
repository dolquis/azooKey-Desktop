#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace azookey::host {

// M32 update notification (docs/sideload-packaging-spec.md section 6.3). The MSI gives the Start
// menu shortcut of the settings app this AppUserModelID; without that shortcut Windows shows no
// toast from an unpackaged process.
inline constexpr wchar_t kAppUserModelId[] = L"dolquis.azooKey";

enum class UpdateToastResult : uint8_t { Shown, DisabledBySystem, Failed };

// The toast XML naming the version, opening url when clicked. Both values are XML-escaped.
// Protocol activation needs no COM activator, so a click from the notification centre works
// after the Host has exited.
std::string BuildUpdateToastXml(std::string_view version, std::string_view url);

// Shows the toast on the calling thread. Failed off Windows, for a url that is not https, and
// when Windows refuses the AppUserModelID (no shortcut). DisabledBySystem when the user turned
// the notifications of the app off.
UpdateToastResult ShowUpdateToast(std::string_view version, std::string_view url);

}  // namespace azookey::host
