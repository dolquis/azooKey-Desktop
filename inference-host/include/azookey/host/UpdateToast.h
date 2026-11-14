#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace azookey::host {

// M32 update notification (docs/sideload-packaging-spec.md section 6.3). The MSI gives the Start
// menu shortcut of the settings app this AppUserModelID; without that shortcut Windows shows no
// toast from an unpackaged process.
inline constexpr wchar_t kAppUserModelId[] = L"dolquis.azooKey";

enum class UpdateToastResult : uint8_t { Shown, DisabledBySystem, Failed };

struct UpdateToastOutcome {
  UpdateToastResult result{UpdateToastResult::Failed};
  // The step that decided the result, a fixed name for the log: input, convert, runtime,
  // activate, load_xml, notifier, setting or show (platform off Windows).
  const char* stage{"input"};
  // The failing HRESULT, 0 unless result is Failed by a Windows Runtime call.
  int32_t hresult{0};
};

// The toast XML naming the version, opening url when clicked. Both values are XML-escaped.
// Protocol activation needs no COM activator, so a click from the notification centre works
// after the Host has exited.
std::string BuildUpdateToastXml(std::string_view version, std::string_view url);

// What ToastNotifier.Setting decides. A setting that could not be read leaves it undecided
// (nullopt: go on and show), since an unpackaged app that never showed a notification may get an
// error there; only a setting read as not enabled stops the toast.
std::optional<UpdateToastResult> ResultForNotificationSetting(bool setting_read, bool enabled);

// Shows the toast on the calling thread. Failed off Windows, for a url outside the repository's
// release pages (kReleasePagePrefix), and when a Windows Runtime call fails. DisabledBySystem when
// Windows reports the notifications of the AppUserModelID as not enabled: the user turned them
// off, or no shortcut carries it. Shown means Show accepted the toast; Windows does not confirm
// that it was displayed.
UpdateToastOutcome ShowUpdateToast(std::string_view version, std::string_view url);

}  // namespace azookey::host
