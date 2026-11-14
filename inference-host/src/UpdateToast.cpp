#include "azookey/host/UpdateToast.h"

#include <climits>

#include "azookey/host/UpdateChecker.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <roapi.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#endif

namespace azookey::host {
namespace {

void AppendEscaped(std::string& out, std::string_view text) {
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&apos;";
        break;
      default:
        out += c;
        break;
    }
  }
}

#ifdef _WIN32
std::wstring WidenUtf8(std::string_view text) {
  if (text.empty() || text.size() > static_cast<size_t>(INT_MAX)) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring wide(static_cast<size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                          wide.data(), length) != length) {
    return {};
  }
  return wide;
}
#endif

}  // namespace

std::string BuildUpdateToastXml(std::string_view version, std::string_view url) {
  std::string xml = R"(<toast activationType="protocol" launch=")";
  AppendEscaped(xml, url);
  xml += R"("><visual><binding template="ToastGeneric"><text>)";
  xml += "azooKey の新しいバージョン v";
  AppendEscaped(xml, version);
  xml += " があります";
  xml += "</text><text>";
  xml += "クリックするとリリースページを開きます。";
  xml += "</text></binding></visual></toast>";
  return xml;
}

std::optional<UpdateToastResult> ResultForNotificationSetting(bool setting_read, bool enabled) {
  if (setting_read && !enabled) return UpdateToastResult::DisabledBySystem;
  return std::nullopt;
}

UpdateToastOutcome ShowUpdateToast(std::string_view version, std::string_view url) {
  // The checker already keeps only release pages; the toast opens nothing else either.
  if (version.empty() || !IsAllowedReleaseUrl(url, kReleasePagePrefix)) return {};
#ifdef _WIN32
  namespace dom = ABI::Windows::Data::Xml::Dom;
  namespace notifications = ABI::Windows::UI::Notifications;
  using Microsoft::WRL::ComPtr;
  using Microsoft::WRL::Wrappers::HStringReference;
  const auto failed = [](const char* stage, HRESULT hr) {
    return UpdateToastOutcome{UpdateToastResult::Failed, stage, static_cast<int32_t>(hr)};
  };

  const auto xml = WidenUtf8(BuildUpdateToastXml(version, url));
  if (xml.empty()) return {UpdateToastResult::Failed, "convert", 0};
  // The update checker's worker is the Host's own thread, so it may join the MTA.
  Microsoft::WRL::Wrappers::RoInitializeWrapper runtime(RO_INIT_MULTITHREADED);
  if (FAILED(static_cast<HRESULT>(runtime)) &&
      static_cast<HRESULT>(runtime) != RPC_E_CHANGED_MODE) {
    return failed("runtime", static_cast<HRESULT>(runtime));
  }

  ComPtr<IInspectable> instance;
  HRESULT hr = RoActivateInstance(
      HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(), &instance);
  if (FAILED(hr)) return failed("activate", hr);
  ComPtr<dom::IXmlDocument> document;
  ComPtr<dom::IXmlDocumentIO> document_io;
  hr = instance.As(&document);
  if (SUCCEEDED(hr)) hr = document.As(&document_io);
  if (SUCCEEDED(hr)) {
    hr = document_io->LoadXml(
        HStringReference(xml.c_str(), static_cast<unsigned int>(xml.size())).Get());
  }
  if (FAILED(hr)) return failed("load_xml", hr);

  ComPtr<notifications::IToastNotificationManagerStatics> manager;
  ComPtr<notifications::IToastNotifier> notifier;
  hr = RoGetActivationFactory(
      HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
      IID_PPV_ARGS(&manager));
  if (SUCCEEDED(hr)) {
    hr = manager->CreateToastNotifierWithId(HStringReference(kAppUserModelId).Get(), &notifier);
  }
  if (FAILED(hr)) return failed("notifier", hr);
  // An AppUserModelID that no Start menu shortcut carries reads as disabled (DisabledForUser on
  // build 26300), so it is not recorded as shown. A setting that cannot be read is undecided.
  notifications::NotificationSetting setting{};
  const bool setting_read = SUCCEEDED(notifier->get_Setting(&setting));
  if (const auto decided = ResultForNotificationSetting(
          setting_read, setting == notifications::NotificationSetting_Enabled)) {
    return {*decided, "setting", 0};
  }

  ComPtr<notifications::IToastNotificationFactory> factory;
  ComPtr<notifications::IToastNotification> toast;
  hr = RoGetActivationFactory(
      HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(),
      IID_PPV_ARGS(&factory));
  if (SUCCEEDED(hr)) hr = factory->CreateToastNotification(document.Get(), &toast);
  if (SUCCEEDED(hr)) hr = notifier->Show(toast.Get());
  if (FAILED(hr)) return failed("show", hr);
  return {UpdateToastResult::Shown, "show", 0};
#else
  return {};
#endif
}

}  // namespace azookey::host
