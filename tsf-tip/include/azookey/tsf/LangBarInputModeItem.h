#pragma once

#include <Windows.h>
#include <ctfutb.h>
#include <msctf.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace azookey::tsf {

// What the taskbar input indicator shows (docs/tsf-deep-integration-spec.md §4.5).
enum class InputModeIndicator : uint8_t {
  Hiragana,      // On, hiragana input: "あ".
  Alphanumeric,  // On, alphanumeric mode: "_A".
  Direct,        // Off: "A".
};

InputModeIndicator ResolveInputModeIndicator(bool keyboard_open, bool alnum_mode);
std::wstring_view InputModeIndicatorText(InputModeIndicator indicator);
std::wstring_view InputModeIndicatorTooltip(InputModeIndicator indicator);

// Glyph color for the taskbar: black on a light taskbar (SystemUsesLightTheme
// nonzero), white otherwise, including when the value is missing or unreadable.
COLORREF InputModeGlyphColor(std::optional<DWORD> system_uses_light_theme);

// HKCU ...\Themes\Personalize\SystemUsesLightTheme, or nullopt.
std::optional<DWORD> ReadSystemUsesLightTheme();

// A square icon of `size` pixels with the indicator's glyph drawn in `color`
// on a transparent background. The caller owns the icon (DestroyIcon).
HICON CreateInputModeIcon(InputModeIndicator indicator, int size, COLORREF color);

// The language bar button that Windows 8 and later show as the IME mode icon
// of the input indicator. Only GUID_LBI_INPUTMODE is shown there.
class LangBarInputModeItem final : public ITfLangBarItemButton, public ITfSource {
 public:
  // Receives OnClick on the TIP thread. A right click is passed on too, so a
  // menu can be added there later (DEV-1738); today the owner ignores it.
  using ClickHandler = std::function<void(TfLBIClick click, POINT point, const RECT* area)>;

  LangBarInputModeItem(const CLSID& text_service, InputModeIndicator indicator,
                       ClickHandler on_click);

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // ITfLangBarItem
  STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* info) override;
  STDMETHODIMP GetStatus(DWORD* status) override;
  STDMETHODIMP Show(BOOL show) override;
  STDMETHODIMP GetTooltipString(BSTR* tooltip) override;

  // ITfLangBarItemButton
  STDMETHODIMP OnClick(TfLBIClick click, POINT point, const RECT* area) override;
  STDMETHODIMP InitMenu(ITfMenu* menu) override;
  STDMETHODIMP OnMenuSelect(UINT id) override;
  STDMETHODIMP GetIcon(HICON* icon) override;
  STDMETHODIMP GetText(BSTR* text) override;

  // ITfSource (ITfLangBarItemSink)
  STDMETHODIMP AdviseSink(REFIID riid, IUnknown* sink, DWORD* cookie) override;
  STDMETHODIMP UnadviseSink(DWORD cookie) override;

  // Notifies the advised sink only when the indicator changes.
  void SetIndicator(InputModeIndicator indicator);
  InputModeIndicator indicator() const { return indicator_; }
  // The owner is going away; later clicks do nothing.
  void Detach() noexcept;

 private:
  ~LangBarInputModeItem();

  LONG ref_count_{1};
  CLSID text_service_;
  InputModeIndicator indicator_;
  ClickHandler on_click_;
  ITfLangBarItemSink* sink_{nullptr};
};

// Adds the item to the thread's language bar on activation and removes it on
// deactivation. A host without ITfLangBarItemMgr gets no item; activation
// does not fail for it.
class InputModeLangBar final {
 public:
  InputModeLangBar() = default;
  InputModeLangBar(const InputModeLangBar&) = delete;
  InputModeLangBar& operator=(const InputModeLangBar&) = delete;
  ~InputModeLangBar();

  HRESULT Add(ITfThreadMgr* thread_mgr, const CLSID& text_service, InputModeIndicator indicator,
              LangBarInputModeItem::ClickHandler on_click);
  void Update(InputModeIndicator indicator);
  HRESULT Remove();
  bool added() const { return item_ != nullptr; }
#ifdef AZOOKEY_TSF_TESTING
  LangBarInputModeItem* item_for_test() const { return item_; }
#endif

 private:
  ITfLangBarItemMgr* manager_{nullptr};
  LangBarInputModeItem* item_{nullptr};
};

}  // namespace azookey::tsf
