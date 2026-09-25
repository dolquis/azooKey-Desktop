#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <gtest/gtest.h>
#include <msctf.h>

#include "azookey/tsf/TextService.h"

// Verifies that TextService::ActivateEx derives its UI-less state from
// ITfThreadMgrEx::GetActiveFlags rather than the dwFlags argument (spec §2.10).

namespace {

class MockKeyboardCompartment final : public ITfCompartment, public ITfSource {
 public:
  explicit MockKeyboardCompartment(bool open) : open_(open) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfCompartment)
      *ppv = static_cast<ITfCompartment*>(this);
    else if (riid == IID_ITfSource)
      *ppv = static_cast<ITfSource*>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    return static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  }
  STDMETHODIMP SetValue(TfClientId tid, const VARIANT* value) override {
    if (!value || value->vt != VT_I4) return E_INVALIDARG;
    last_writer_ = tid;
    SetOpen(value->lVal != 0);
    return S_OK;
  }
  STDMETHODIMP GetValue(VARIANT* value) override {
    if (!value) return E_INVALIDARG;
    VariantInit(value);
    value->vt = VT_I4;
    value->lVal = open_ ? 1 : 0;
    return S_OK;
  }
  STDMETHODIMP AdviseSink(REFIID riid, IUnknown* sink, DWORD* cookie) override {
    if (!sink || !cookie) return E_POINTER;
    if (riid != IID_ITfCompartmentEventSink) return E_NOINTERFACE;
    if (sink_) return E_UNEXPECTED;
    const HRESULT hr =
        sink->QueryInterface(IID_ITfCompartmentEventSink, reinterpret_cast<void**>(&sink_));
    if (FAILED(hr)) return hr;
    *cookie = kCookie;
    ++advise_count_;
    return S_OK;
  }
  STDMETHODIMP UnadviseSink(DWORD cookie) override {
    if (cookie != kCookie || !sink_) return E_INVALIDARG;
    sink_->Release();
    sink_ = nullptr;
    ++unadvise_count_;
    return S_OK;
  }
  void SetOpen(bool open) {
    open_ = open;
    if (sink_) sink_->OnChange(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
  }
  int advise_count() const { return advise_count_; }
  int unadvise_count() const { return unadvise_count_; }
  TfClientId last_writer() const { return last_writer_; }

 private:
  static constexpr DWORD kCookie = 0x516;
  LONG ref_count_{1};
  bool open_;
  ITfCompartmentEventSink* sink_{nullptr};
  int advise_count_{0};
  int unadvise_count_{0};
  TfClientId last_writer_{TF_CLIENTID_NULL};
};

// Minimal thread manager mock implementing the interfaces ActivateEx touches:
// ITfThreadMgrEx (for GetActiveFlags), ITfKeystrokeMgr and ITfSource (so the
// sink advise succeeds and ActivateEx returns S_OK). GetActiveFlags returns a
// caller-configured value so each test can simulate UI-less / non-UI-less.
class MockThreadMgrEx final : public ITfThreadMgrEx,
                              public ITfKeystrokeMgr,
                              public ITfCompartmentMgr,
                              public ITfSource {
 public:
  explicit MockThreadMgrEx(DWORD active_flags, bool keyboard_open = true)
      : active_flags_(active_flags), keyboard_compartment(keyboard_open) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfThreadMgr || riid == IID_ITfThreadMgrEx) {
      *ppv = static_cast<ITfThreadMgrEx*>(this);
    } else if (riid == IID_ITfKeystrokeMgr) {
      *ppv = static_cast<ITfKeystrokeMgr*>(this);
    } else if (riid == IID_ITfSource) {
      *ppv = static_cast<ITfSource*>(this);
    } else if (riid == IID_ITfCompartmentMgr) {
      *ppv = static_cast<ITfCompartmentMgr*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    return static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  }

  // ITfThreadMgr
  STDMETHODIMP Activate(TfClientId* ptid) override {
    if (ptid) *ptid = client_id;
    return S_OK;
  }
  STDMETHODIMP Deactivate() override { return S_OK; }
  STDMETHODIMP CreateDocumentMgr(ITfDocumentMgr** pp) override { if (pp) *pp = nullptr; return E_NOTIMPL; }
  STDMETHODIMP EnumDocumentMgrs(IEnumTfDocumentMgrs** pp) override { if (pp) *pp = nullptr; return E_NOTIMPL; }
  STDMETHODIMP GetFocus(ITfDocumentMgr** pp) override { if (pp) *pp = nullptr; return E_NOTIMPL; }
  STDMETHODIMP SetFocus(ITfDocumentMgr*) override { return E_NOTIMPL; }
  STDMETHODIMP AssociateFocus(HWND, ITfDocumentMgr*, ITfDocumentMgr** pp) override {
    if (pp) *pp = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP IsThreadFocus(BOOL* pf) override { if (pf) *pf = FALSE; return S_OK; }
  STDMETHODIMP GetFunctionProvider(REFCLSID, ITfFunctionProvider** pp) override {
    if (pp) *pp = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP EnumFunctionProviders(IEnumTfFunctionProviders** pp) override {
    if (pp) *pp = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetGlobalCompartment(ITfCompartmentMgr** pp) override { if (pp) *pp = nullptr; return E_NOTIMPL; }

  // ITfCompartmentMgr
  STDMETHODIMP GetCompartment(REFGUID guid, ITfCompartment** pp) override {
    if (!pp) return E_INVALIDARG;
    *pp = nullptr;
    if (guid != GUID_COMPARTMENT_KEYBOARD_OPENCLOSE) return E_INVALIDARG;
    *pp = &keyboard_compartment;
    keyboard_compartment.AddRef();
    return S_OK;
  }
  STDMETHODIMP ClearCompartment(TfClientId, REFGUID) override { return E_NOTIMPL; }
  STDMETHODIMP EnumCompartments(IEnumGUID** pp) override {
    if (pp) *pp = nullptr;
    return E_NOTIMPL;
  }

  // ITfThreadMgrEx
  STDMETHODIMP ActivateEx(TfClientId* ptid, DWORD) override {
    if (ptid) *ptid = client_id;
    return S_OK;
  }
  STDMETHODIMP GetActiveFlags(DWORD* pdwFlags) override {
    if (!pdwFlags) return E_POINTER;
    *pdwFlags = active_flags_;
    return S_OK;
  }

  // ITfKeystrokeMgr — only advise/unadvise need to succeed.
  STDMETHODIMP AdviseKeyEventSink(TfClientId, ITfKeyEventSink* sink, BOOL) override {
    if (!sink) return E_POINTER;
    key_sink_ = sink;
    key_sink_->AddRef();
    return S_OK;
  }
  STDMETHODIMP UnadviseKeyEventSink(TfClientId) override {
    if (key_sink_) { key_sink_->Release(); key_sink_ = nullptr; }
    return S_OK;
  }
  STDMETHODIMP GetForeground(CLSID* p) override { if (p) *p = GUID_NULL; return S_OK; }
  STDMETHODIMP TestKeyDown(WPARAM, LPARAM, BOOL* p) override { if (p) *p = FALSE; return E_NOTIMPL; }
  STDMETHODIMP TestKeyUp(WPARAM, LPARAM, BOOL* p) override { if (p) *p = FALSE; return E_NOTIMPL; }
  STDMETHODIMP KeyDown(WPARAM, LPARAM, BOOL* p) override { if (p) *p = FALSE; return E_NOTIMPL; }
  STDMETHODIMP KeyUp(WPARAM, LPARAM, BOOL* p) override { if (p) *p = FALSE; return E_NOTIMPL; }
  STDMETHODIMP GetPreservedKey(ITfContext*, const TF_PRESERVEDKEY*, GUID* p) override {
    if (p) *p = GUID_NULL;
    return E_NOTIMPL;
  }
  STDMETHODIMP IsPreservedKey(REFGUID, const TF_PRESERVEDKEY*, BOOL* p) override {
    if (p) *p = FALSE;
    return E_NOTIMPL;
  }
  STDMETHODIMP PreserveKey(TfClientId, REFGUID, const TF_PRESERVEDKEY*, const WCHAR*, ULONG) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP UnpreserveKey(REFGUID, const TF_PRESERVEDKEY*) override { return E_NOTIMPL; }
  STDMETHODIMP SetPreservedKeyDescription(REFGUID, const WCHAR*, ULONG) override { return E_NOTIMPL; }
  STDMETHODIMP GetPreservedKeyDescription(REFGUID, BSTR* p) override { if (p) *p = nullptr; return E_NOTIMPL; }
  STDMETHODIMP SimulatePreservedKey(ITfContext*, REFGUID, BOOL* p) override {
    if (p) *p = FALSE;
    return E_NOTIMPL;
  }

  // ITfSource — only the thread-manager-event-sink advise needs to succeed.
  STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD* cookie) override {
    if (!punk || !cookie) return E_POINTER;
    if (riid != IID_ITfThreadMgrEventSink) return E_NOINTERFACE;
    thread_sink_ = punk;
    thread_sink_->AddRef();
    *cookie = kSinkCookie;
    return S_OK;
  }
  STDMETHODIMP UnadviseSink(DWORD cookie) override {
    if (thread_sink_) { thread_sink_->Release(); thread_sink_ = nullptr; }
    return cookie == kSinkCookie ? S_OK : E_INVALIDARG;
  }

  TfClientId client_id{7};
  MockKeyboardCompartment keyboard_compartment;

 private:
  static constexpr DWORD kSinkCookie = 0x515;
  LONG ref_count_{1};
  DWORD active_flags_{0};
  ITfKeyEventSink* key_sink_{nullptr};
  IUnknown* thread_sink_{nullptr};
};

bool ActivateAndReadUiLess(DWORD active_flags) {
  azookey::tsf::TextService service;
  MockThreadMgrEx mock(active_flags);
  EXPECT_TRUE(SUCCEEDED(service.ActivateEx(&mock, mock.client_id, 0)));
  const bool ui_less = service.ui_less_mode();
  service.Deactivate();
  return ui_less;
}

}  // namespace

TEST(TsfTipActivateUiLessTest, UiElementEnabledOnlyFlagSetsUiLessMode) {
  EXPECT_TRUE(ActivateAndReadUiLess(TF_TMF_UIELEMENTENABLEDONLY));
}

TEST(TsfTipActivateUiLessTest, NoUiElementFlagLeavesUiLessModeFalse) {
  EXPECT_FALSE(ActivateAndReadUiLess(0));
}

TEST(TsfTipActivateUiLessTest, KeyboardCompartmentInitialStateAndNotifications) {
  azookey::tsf::TextService service;
  MockThreadMgrEx mock(0, /*keyboard_open=*/false);
  ASSERT_EQ(service.ActivateEx(&mock, mock.client_id, 0), S_OK);
  EXPECT_FALSE(service.keyboard_open());
  EXPECT_EQ(mock.keyboard_compartment.advise_count(), 1);

  mock.keyboard_compartment.SetOpen(true);
  EXPECT_TRUE(service.keyboard_open());
  mock.keyboard_compartment.SetOpen(false);
  EXPECT_FALSE(service.keyboard_open());
  EXPECT_EQ(service.OnChange(GUID_NULL), S_OK);
  EXPECT_FALSE(service.keyboard_open());

  EXPECT_EQ(service.Deactivate(), S_OK);
  EXPECT_EQ(mock.keyboard_compartment.unadvise_count(), 1);
  mock.keyboard_compartment.SetOpen(false);
  EXPECT_TRUE(service.keyboard_open());
}

TEST(TsfTipActivateUiLessTest, ToggleKeyboardOpenWritesCompartment) {
  azookey::tsf::TextService service;
  MockThreadMgrEx mock(0);
  EXPECT_EQ(service.ToggleKeyboardOpen(), E_UNEXPECTED);
  ASSERT_EQ(service.ActivateEx(&mock, mock.client_id, 0), S_OK);

  ASSERT_EQ(service.ToggleKeyboardOpen(), S_OK);
  EXPECT_FALSE(service.keyboard_open());
  EXPECT_EQ(mock.keyboard_compartment.last_writer(), mock.client_id);
  ASSERT_EQ(service.ToggleKeyboardOpen(), S_OK);
  EXPECT_TRUE(service.keyboard_open());

  EXPECT_EQ(service.Deactivate(), S_OK);
  EXPECT_EQ(service.ToggleKeyboardOpen(), E_UNEXPECTED);
}
