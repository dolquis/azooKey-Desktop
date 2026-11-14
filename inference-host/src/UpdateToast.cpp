#include "azookey/host/UpdateToast.h"

#include <climits>

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

UpdateToastResult ShowUpdateToast(std::string_view version, std::string_view url) {
  if (version.empty() || !url.starts_with("https://")) return UpdateToastResult::Failed;
#ifdef _WIN32
  namespace dom = ABI::Windows::Data::Xml::Dom;
  namespace notifications = ABI::Windows::UI::Notifications;
  using Microsoft::WRL::ComPtr;
  using Microsoft::WRL::Wrappers::HStringReference;

  const auto xml = WidenUtf8(BuildUpdateToastXml(version, url));
  if (xml.empty()) return UpdateToastResult::Failed;
  // The update checker's worker is the Host's own thread, so it may join the MTA.
  Microsoft::WRL::Wrappers::RoInitializeWrapper runtime(RO_INIT_MULTITHREADED);
  if (FAILED(static_cast<HRESULT>(runtime)) &&
      static_cast<HRESULT>(runtime) != RPC_E_CHANGED_MODE) {
    return UpdateToastResult::Failed;
  }

  ComPtr<IInspectable> instance;
  if (FAILED(RoActivateInstance(
          HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(), &instance))) {
    return UpdateToastResult::Failed;
  }
  ComPtr<dom::IXmlDocument> document;
  ComPtr<dom::IXmlDocumentIO> document_io;
  if (FAILED(instance.As(&document)) || FAILED(document.As(&document_io)) ||
      FAILED(document_io->LoadXml(
          HStringReference(xml.c_str(), static_cast<unsigned int>(xml.size())).Get()))) {
    return UpdateToastResult::Failed;
  }

  ComPtr<notifications::IToastNotificationManagerStatics> manager;
  ComPtr<notifications::IToastNotifier> notifier;
  if (FAILED(RoGetActivationFactory(
          HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
          IID_PPV_ARGS(&manager))) ||
      FAILED(
          manager->CreateToastNotifierWithId(HStringReference(kAppUserModelId).Get(), &notifier))) {
    return UpdateToastResult::Failed;
  }
  // An AppUserModelID without its Start menu shortcut fails here.
  notifications::NotificationSetting setting{};
  if (FAILED(notifier->get_Setting(&setting))) return UpdateToastResult::Failed;
  if (setting != notifications::NotificationSetting_Enabled) {
    return UpdateToastResult::DisabledBySystem;
  }

  ComPtr<notifications::IToastNotificationFactory> factory;
  ComPtr<notifications::IToastNotification> toast;
  if (FAILED(RoGetActivationFactory(
          HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(),
          IID_PPV_ARGS(&factory))) ||
      FAILED(factory->CreateToastNotification(document.Get(), &toast)) ||
      FAILED(notifier->Show(toast.Get()))) {
    return UpdateToastResult::Failed;
  }
  return UpdateToastResult::Shown;
#else
  return UpdateToastResult::Failed;
#endif
}

}  // namespace azookey::host
