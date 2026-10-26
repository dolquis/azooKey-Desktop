#include "azookey/tsf/LangBarInputModeItem.h"

#include <ctffunc.h>
#include <olectl.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <new>
#include <utility>
#include <vector>

#include "azookey/tsf/DpiScaling.h"

namespace azookey::tsf {
namespace {

constexpr DWORD kSinkCookie = 0x1B7;

// szDescription holds at most TF_LBI_DESC_MAXLEN characters.
constexpr wchar_t kItemDescription[] = L"azooKey 入力モード";
static_assert(std::size(kItemDescription) <= TF_LBI_DESC_MAXLEN);

BSTR AllocBstr(std::wstring_view text) {
  return SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
}

}  // namespace

InputModeIndicator ResolveInputModeIndicator(bool keyboard_open, bool alnum_mode) {
  if (!keyboard_open) return InputModeIndicator::Direct;
  return alnum_mode ? InputModeIndicator::Alphanumeric : InputModeIndicator::Hiragana;
}

std::wstring_view InputModeIndicatorText(InputModeIndicator indicator) {
  switch (indicator) {
    case InputModeIndicator::Hiragana:
      return L"あ";
    case InputModeIndicator::Alphanumeric:
      return L"_A";
    case InputModeIndicator::Direct:
      break;
  }
  return L"A";
}

std::wstring_view InputModeIndicatorTooltip(InputModeIndicator indicator) {
  switch (indicator) {
    case InputModeIndicator::Hiragana:
      return L"azooKey: ひらがな";
    case InputModeIndicator::Alphanumeric:
      return L"azooKey: 半角英数";
    case InputModeIndicator::Direct:
      break;
  }
  return L"azooKey: 直接入力";
}

COLORREF InputModeGlyphColor(std::optional<DWORD> system_uses_light_theme) {
  return system_uses_light_theme && *system_uses_light_theme != 0 ? RGB(0, 0, 0)
                                                                  : RGB(255, 255, 255);
}

std::optional<DWORD> ReadSystemUsesLightTheme() {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (RegGetValueW(
          HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
    return std::nullopt;
  return value;
}

HICON CreateInputModeIcon(InputModeIndicator indicator, int size, COLORREF color) {
  if (size <= 0 || size > 256) return nullptr;
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = size;
  info.bmiHeader.biHeight = -size;  // Top-down rows.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  HDC dc = CreateCompatibleDC(nullptr);
  if (!dc) return nullptr;
  void* bits = nullptr;
  HBITMAP color_bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  HBITMAP mask_bitmap = CreateBitmap(size, size, 1, 1, nullptr);
  HICON icon = nullptr;
  if (color_bitmap && mask_bitmap && bits) {
    auto* pixels = static_cast<uint32_t*>(bits);
    std::fill(pixels, pixels + static_cast<size_t>(size) * size, 0u);
    const HGDIOBJ old_bitmap = SelectObject(dc, color_bitmap);
    // Draw white on black; the brightness of each pixel becomes its coverage.
    const bool underline = indicator == InputModeIndicator::Alphanumeric;
    const int line = std::max(1, size / 16);
    RECT text_rect{0, 0, size, underline ? size - 2 * line : size};
    const int glyph_height =
        indicator == InputModeIndicator::Hiragana ? size * 7 / 8 : size * 3 / 4;
    HFONT font = CreateFontW(-glyph_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Yu Gothic UI");
    const HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    const std::wstring_view glyph =
        indicator == InputModeIndicator::Hiragana ? std::wstring_view(L"あ") : L"A";
    DrawTextW(dc, glyph.data(), static_cast<int>(glyph.size()), &text_rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (underline) {
      const RECT bar{size / 8, size - line, size - size / 8, size};
      FillRect(dc, &bar, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    }
    GdiFlush();
    if (old_font) SelectObject(dc, old_font);
    if (font) DeleteObject(font);
    SelectObject(dc, old_bitmap);

    const uint32_t rgb = (static_cast<uint32_t>(GetRValue(color)) << 16) |
                         (static_cast<uint32_t>(GetGValue(color)) << 8) | GetBValue(color);
    for (size_t i = 0; i < static_cast<size_t>(size) * size; ++i) {
      const uint32_t p = pixels[i];
      const uint32_t coverage = std::max({(p >> 16) & 0xFFu, (p >> 8) & 0xFFu, p & 0xFFu});
      pixels[i] = coverage ? (coverage << 24) | rgb : 0u;
    }

    // An all-zero AND mask leaves transparency to the alpha channel.
    std::vector<uint8_t> mask_bits(static_cast<size_t>((size + 15) / 16 * 2) * size, 0);
    SetBitmapBits(mask_bitmap, static_cast<DWORD>(mask_bits.size()), mask_bits.data());
    ICONINFO icon_info{};
    icon_info.fIcon = TRUE;
    icon_info.hbmMask = mask_bitmap;
    icon_info.hbmColor = color_bitmap;
    icon = CreateIconIndirect(&icon_info);
  }
  if (mask_bitmap) DeleteObject(mask_bitmap);
  if (color_bitmap) DeleteObject(color_bitmap);
  DeleteDC(dc);
  return icon;
}

LangBarInputModeItem::LangBarInputModeItem(const CLSID& text_service, InputModeIndicator indicator,
                                           ClickHandler on_click)
    : text_service_(text_service), indicator_(indicator), on_click_(std::move(on_click)) {}

LangBarInputModeItem::~LangBarInputModeItem() {
  if (sink_) sink_->Release();
}

STDMETHODIMP LangBarInputModeItem::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_INVALIDARG;
  *ppv = nullptr;
  if (riid == IID_IUnknown || riid == IID_ITfLangBarItem || riid == IID_ITfLangBarItemButton)
    *ppv = static_cast<ITfLangBarItemButton*>(this);
  else if (riid == IID_ITfSource)
    *ppv = static_cast<ITfSource*>(this);
  else
    return E_NOINTERFACE;
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) LangBarInputModeItem::AddRef() {
  return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
}

STDMETHODIMP_(ULONG) LangBarInputModeItem::Release() {
  const LONG count = InterlockedDecrement(&ref_count_);
  if (count == 0) delete this;
  return static_cast<ULONG>(count);
}

STDMETHODIMP LangBarInputModeItem::GetInfo(TF_LANGBARITEMINFO* info) {
  if (!info) return E_INVALIDARG;
  info->clsidService = text_service_;
  info->guidItem = GUID_LBI_INPUTMODE;
  info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY;
  info->ulSort = 0;
  wcsncpy_s(info->szDescription, kItemDescription, _TRUNCATE);
  return S_OK;
}

STDMETHODIMP LangBarInputModeItem::GetStatus(DWORD* status) {
  if (!status) return E_INVALIDARG;
  *status = 0;
  return S_OK;
}

STDMETHODIMP LangBarInputModeItem::Show(BOOL show) {
  UNREFERENCED_PARAMETER(show);
  return E_NOTIMPL;
}

STDMETHODIMP LangBarInputModeItem::GetTooltipString(BSTR* tooltip) {
  if (!tooltip) return E_INVALIDARG;
  *tooltip = AllocBstr(InputModeIndicatorTooltip(indicator_));
  return *tooltip ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP LangBarInputModeItem::OnClick(TfLBIClick click, POINT point, const RECT* area) try {
  if (on_click_) {
    // The handler may Detach this item; keep both alive for the call, also
    // when it throws.
    struct Hold {
      LangBarInputModeItem* item;
      explicit Hold(LangBarInputModeItem* held) : item(held) { item->AddRef(); }
      ~Hold() { item->Release(); }
      Hold(const Hold&) = delete;
      Hold& operator=(const Hold&) = delete;
    } hold(this);
    const ClickHandler handler = on_click_;
    handler(click, point, area);
  }
  return S_OK;
} catch (const std::bad_alloc&) {
  return E_OUTOFMEMORY;
} catch (...) {
  return E_FAIL;
}

STDMETHODIMP LangBarInputModeItem::InitMenu(ITfMenu* menu) {
  UNREFERENCED_PARAMETER(menu);
  return E_NOTIMPL;
}

STDMETHODIMP LangBarInputModeItem::OnMenuSelect(UINT id) {
  UNREFERENCED_PARAMETER(id);
  return E_NOTIMPL;
}

STDMETHODIMP LangBarInputModeItem::GetIcon(HICON* icon) {
  if (!icon) return E_INVALIDARG;
  // The input indicator's mode icon is 16x16 at 96 DPI and follows DPI.
  *icon = CreateInputModeIcon(indicator_, ScaleForDpi(16, GetDpiForSystem()),
                              InputModeGlyphColor(ReadSystemUsesLightTheme()));
  return *icon ? S_OK : E_FAIL;
}

STDMETHODIMP LangBarInputModeItem::GetText(BSTR* text) {
  if (!text) return E_INVALIDARG;
  *text = AllocBstr(InputModeIndicatorText(indicator_));
  return *text ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP LangBarInputModeItem::AdviseSink(REFIID riid, IUnknown* sink, DWORD* cookie) {
  if (!sink || !cookie) return E_INVALIDARG;
  *cookie = 0;
  if (riid != IID_ITfLangBarItemSink) return CONNECT_E_CANNOTCONNECT;
  if (sink_) return CONNECT_E_ADVISELIMIT;
  const HRESULT hr = sink->QueryInterface(IID_ITfLangBarItemSink, reinterpret_cast<void**>(&sink_));
  if (FAILED(hr)) {
    sink_ = nullptr;
    return E_NOINTERFACE;
  }
  *cookie = kSinkCookie;
  return S_OK;
}

STDMETHODIMP LangBarInputModeItem::UnadviseSink(DWORD cookie) {
  if (cookie != kSinkCookie || !sink_) return CONNECT_E_NOCONNECTION;
  sink_->Release();
  sink_ = nullptr;
  return S_OK;
}

void LangBarInputModeItem::SetIndicator(InputModeIndicator indicator) {
  if (indicator == indicator_) return;
  indicator_ = indicator;
  if (sink_) (void)sink_->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP);
}

void LangBarInputModeItem::Detach() noexcept { on_click_ = nullptr; }

InputModeLangBar::~InputModeLangBar() { (void)Remove(); }

HRESULT InputModeLangBar::Add(ITfThreadMgr* thread_mgr, const CLSID& text_service,
                              InputModeIndicator indicator,
                              LangBarInputModeItem::ClickHandler on_click) {
  if (!thread_mgr) return E_INVALIDARG;
  if (item_) return S_FALSE;
  ITfLangBarItemMgr* manager = nullptr;
  HRESULT hr =
      thread_mgr->QueryInterface(IID_ITfLangBarItemMgr, reinterpret_cast<void**>(&manager));
  if (FAILED(hr) || !manager) return FAILED(hr) ? hr : E_NOINTERFACE;
  auto* item =
      new (std::nothrow) LangBarInputModeItem(text_service, indicator, std::move(on_click));
  if (!item) {
    manager->Release();
    return E_OUTOFMEMORY;
  }
  hr = manager->AddItem(item);
  if (FAILED(hr)) {
    item->Detach();
    item->Release();
    manager->Release();
    return hr;
  }
  manager_ = manager;
  item_ = item;
  return S_OK;
}

void InputModeLangBar::Update(InputModeIndicator indicator) {
  if (item_) item_->SetIndicator(indicator);
}

HRESULT InputModeLangBar::Remove() {
  if (!item_) return S_OK;
  item_->Detach();
  const HRESULT hr = manager_ ? manager_->RemoveItem(item_) : S_OK;
  item_->Release();
  item_ = nullptr;
  if (manager_) {
    manager_->Release();
    manager_ = nullptr;
  }
  return hr;
}

}  // namespace azookey::tsf
