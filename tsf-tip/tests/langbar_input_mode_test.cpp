#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ctffunc.h>
#include <ctfutb.h>
#include <gtest/gtest.h>
#include <msctf.h>
#include <olectl.h>

#include <vector>

#include "azookey/tsf/LangBarInputModeItem.h"

// The input indicator button of docs/tsf-deep-integration-spec.md §4.5.

namespace {

using azookey::tsf::InputModeIndicator;
using azookey::tsf::LangBarInputModeItem;

constexpr CLSID kTestClsid = {
    0x1d2c3b4a, 0x5e6f, 0x4a7b, {0x8c, 0x9d, 0xae, 0xbf, 0xc0, 0xd1, 0xe2, 0xf3}};

class CountingSink final : public ITfLangBarItemSink {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid != IID_IUnknown && riid != IID_ITfLangBarItemSink) return E_NOINTERFACE;
    *ppv = static_cast<ITfLangBarItemSink*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs); }
  STDMETHODIMP_(ULONG) Release() override { return static_cast<ULONG>(--refs); }
  STDMETHODIMP OnUpdate(DWORD flags) override {
    ++updates;
    last_flags = flags;
    return S_OK;
  }
  long refs{1};
  int updates{0};
  DWORD last_flags{0};
};

struct Click {
  TfLBIClick click;
  POINT point;
};

}  // namespace

TEST(InputModeIndicatorTest, ResolvesFromOpenAndAlphanumericState) {
  EXPECT_EQ(azookey::tsf::ResolveInputModeIndicator(true, false), InputModeIndicator::Hiragana);
  EXPECT_EQ(azookey::tsf::ResolveInputModeIndicator(true, true), InputModeIndicator::Alphanumeric);
  EXPECT_EQ(azookey::tsf::ResolveInputModeIndicator(false, false), InputModeIndicator::Direct);
  EXPECT_EQ(azookey::tsf::ResolveInputModeIndicator(false, true), InputModeIndicator::Direct);
}

TEST(InputModeIndicatorTest, TextAndTooltipTellEveryStateApart) {
  EXPECT_EQ(azookey::tsf::InputModeIndicatorText(InputModeIndicator::Hiragana), L"あ");
  EXPECT_EQ(azookey::tsf::InputModeIndicatorText(InputModeIndicator::Alphanumeric), L"_A");
  EXPECT_EQ(azookey::tsf::InputModeIndicatorText(InputModeIndicator::Direct), L"A");
  EXPECT_NE(azookey::tsf::InputModeIndicatorTooltip(InputModeIndicator::Hiragana),
            azookey::tsf::InputModeIndicatorTooltip(InputModeIndicator::Alphanumeric));
  EXPECT_NE(azookey::tsf::InputModeIndicatorTooltip(InputModeIndicator::Alphanumeric),
            azookey::tsf::InputModeIndicatorTooltip(InputModeIndicator::Direct));
}

TEST(InputModeIndicatorTest, GlyphIsWhiteUnlessTheTaskbarIsLight) {
  // A missing or unreadable SystemUsesLightTheme is taken as a dark taskbar.
  EXPECT_EQ(azookey::tsf::InputModeGlyphColor(std::nullopt), RGB(255, 255, 255));
  EXPECT_EQ(azookey::tsf::InputModeGlyphColor(0u), RGB(255, 255, 255));
  EXPECT_EQ(azookey::tsf::InputModeGlyphColor(1u), RGB(0, 0, 0));
}

TEST(InputModeIndicatorTest, IconHasTheRequestedSizeAndAnAlphaChannel) {
  for (const auto indicator : {InputModeIndicator::Hiragana, InputModeIndicator::Alphanumeric,
                               InputModeIndicator::Direct}) {
    for (const int size : {16, 24, 32}) {
      SCOPED_TRACE(size);
      HICON icon = azookey::tsf::CreateInputModeIcon(indicator, size, RGB(255, 255, 255));
      ASSERT_NE(icon, nullptr);
      ICONINFO info{};
      ASSERT_TRUE(GetIconInfo(icon, &info));
      BITMAP bitmap{};
      ASSERT_NE(GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap), 0);
      EXPECT_EQ(bitmap.bmWidth, size);
      EXPECT_EQ(bitmap.bmHeight, size);

      BITMAPINFO request{};
      request.bmiHeader.biSize = sizeof(request.bmiHeader);
      request.bmiHeader.biWidth = size;
      request.bmiHeader.biHeight = -size;
      request.bmiHeader.biPlanes = 1;
      request.bmiHeader.biBitCount = 32;
      request.bmiHeader.biCompression = BI_RGB;
      std::vector<uint32_t> pixels(static_cast<size_t>(size) * size);
      HDC dc = GetDC(nullptr);
      ASSERT_EQ(GetDIBits(dc, info.hbmColor, 0, size, pixels.data(), &request, DIB_RGB_COLORS),
                size);
      ReleaseDC(nullptr, dc);
      size_t opaque = 0;
      for (const uint32_t pixel : pixels) {
        if ((pixel >> 24) != 0) ++opaque;
      }
      // The glyph covers part of the square, never all of it.
      EXPECT_GT(opaque, 0u);
      EXPECT_LT(opaque, pixels.size());
      // The corner stays transparent so the taskbar shows through.
      EXPECT_EQ(pixels.front() >> 24, 0u);

      DeleteObject(info.hbmColor);
      DeleteObject(info.hbmMask);
      DestroyIcon(icon);
    }
  }
  EXPECT_EQ(azookey::tsf::CreateInputModeIcon(InputModeIndicator::Direct, 0, 0), nullptr);
}

TEST(LangBarInputModeItemTest, ReportsTheInputModeItemOfTheTextService) {
  auto* item = new LangBarInputModeItem(kTestClsid, InputModeIndicator::Hiragana, nullptr);
  TF_LANGBARITEMINFO info{};
  ASSERT_EQ(item->GetInfo(&info), S_OK);
  EXPECT_TRUE(IsEqualGUID(info.guidItem, GUID_LBI_INPUTMODE));
  EXPECT_TRUE(IsEqualGUID(info.clsidService, kTestClsid));
  EXPECT_NE(info.dwStyle & TF_LBI_STYLE_BTN_BUTTON, 0u);
  EXPECT_NE(info.szDescription[0], L'\0');
  DWORD status = 1;
  ASSERT_EQ(item->GetStatus(&status), S_OK);
  EXPECT_EQ(status, 0u);
  HICON icon = nullptr;
  ASSERT_EQ(item->GetIcon(&icon), S_OK);
  EXPECT_NE(icon, nullptr);
  DestroyIcon(icon);
  BSTR text = nullptr;
  ASSERT_EQ(item->GetText(&text), S_OK);
  EXPECT_STREQ(text, L"あ");
  SysFreeString(text);
  BSTR tooltip = nullptr;
  ASSERT_EQ(item->GetTooltipString(&tooltip), S_OK);
  EXPECT_NE(SysStringLen(tooltip), 0u);
  SysFreeString(tooltip);
  EXPECT_EQ(item->Release(), 0u);
}

TEST(LangBarInputModeItemTest, NotifiesTheSinkOnlyWhenTheIndicatorChanges) {
  CountingSink sink;
  auto* item = new LangBarInputModeItem(kTestClsid, InputModeIndicator::Hiragana, nullptr);
  ITfSource* source = nullptr;
  ASSERT_EQ(item->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source)), S_OK);
  DWORD cookie = 0;
  ASSERT_EQ(source->AdviseSink(IID_ITfLangBarItemSink, &sink, &cookie), S_OK);
  DWORD second = 0;
  EXPECT_EQ(source->AdviseSink(IID_ITfLangBarItemSink, &sink, &second), CONNECT_E_ADVISELIMIT);
  EXPECT_EQ(source->AdviseSink(IID_ITfCompartmentEventSink, &sink, &second),
            CONNECT_E_CANNOTCONNECT);

  item->SetIndicator(InputModeIndicator::Hiragana);
  EXPECT_EQ(sink.updates, 0);
  item->SetIndicator(InputModeIndicator::Direct);
  EXPECT_EQ(sink.updates, 1);
  EXPECT_EQ(sink.last_flags, static_cast<DWORD>(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP));
  item->SetIndicator(InputModeIndicator::Alphanumeric);
  EXPECT_EQ(sink.updates, 2);

  EXPECT_EQ(source->UnadviseSink(cookie + 1), CONNECT_E_NOCONNECTION);
  ASSERT_EQ(source->UnadviseSink(cookie), S_OK);
  EXPECT_EQ(sink.refs, 1);
  item->SetIndicator(InputModeIndicator::Direct);
  EXPECT_EQ(sink.updates, 2);
  source->Release();
  EXPECT_EQ(item->Release(), 0u);
}

TEST(LangBarInputModeItemTest, ReleasingTheItemReleasesAnAdvisedSink) {
  CountingSink sink;
  auto* item = new LangBarInputModeItem(kTestClsid, InputModeIndicator::Direct, nullptr);
  DWORD cookie = 0;
  ASSERT_EQ(item->AdviseSink(IID_ITfLangBarItemSink, &sink, &cookie), S_OK);
  EXPECT_EQ(sink.refs, 2);
  EXPECT_EQ(item->Release(), 0u);
  EXPECT_EQ(sink.refs, 1);
}

TEST(LangBarInputModeItemTest, PassesClicksToTheOwnerUntilDetached) {
  std::vector<Click> clicks;
  auto* item = new LangBarInputModeItem(
      kTestClsid, InputModeIndicator::Hiragana,
      [&clicks](TfLBIClick click, POINT point, const RECT*) { clicks.push_back({click, point}); });
  ASSERT_EQ(item->OnClick(TF_LBI_CLK_LEFT, POINT{3, 4}, nullptr), S_OK);
  ASSERT_EQ(item->OnClick(TF_LBI_CLK_RIGHT, POINT{5, 6}, nullptr), S_OK);
  ASSERT_EQ(clicks.size(), 2u);
  EXPECT_EQ(clicks[0].click, TF_LBI_CLK_LEFT);
  EXPECT_EQ(clicks[0].point.x, 3);
  EXPECT_EQ(clicks[1].click, TF_LBI_CLK_RIGHT);
  item->Detach();
  ASSERT_EQ(item->OnClick(TF_LBI_CLK_LEFT, POINT{}, nullptr), S_OK);
  EXPECT_EQ(clicks.size(), 2u);
  EXPECT_EQ(item->Release(), 0u);
}

TEST(LangBarInputModeItemTest, ExposesOnlyTheButtonAndSourceInterfaces) {
  auto* item = new LangBarInputModeItem(kTestClsid, InputModeIndicator::Hiragana, nullptr);
  for (const IID& iid :
       {IID_IUnknown, IID_ITfLangBarItem, IID_ITfLangBarItemButton, IID_ITfSource}) {
    IUnknown* unknown = nullptr;
    ASSERT_EQ(item->QueryInterface(iid, reinterpret_cast<void**>(&unknown)), S_OK);
    unknown->Release();
  }
  IUnknown* other = reinterpret_cast<IUnknown*>(1);
  EXPECT_EQ(item->QueryInterface(IID_ITfLangBarItemBitmapButton, reinterpret_cast<void**>(&other)),
            E_NOINTERFACE);
  EXPECT_EQ(other, nullptr);
  EXPECT_EQ(item->Release(), 0u);
}
