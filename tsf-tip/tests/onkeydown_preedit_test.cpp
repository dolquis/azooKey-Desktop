#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <gtest/gtest.h>
#include <msctf.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "azookey/ipc/Limits.h"
#include "azookey/tsf/TextService.h"

namespace {

class KeyboardStateGuard {
 public:
  KeyboardStateGuard() {
    GetKeyboardState(original_.data());
    ClearSystemModifiers();
  }

  ~KeyboardStateGuard() { SetKeyboardState(original_.data()); }

  void SetDown(int vk, bool down) {
    std::array<BYTE, 256> state{};
    GetKeyboardState(state.data());
    const auto index = static_cast<size_t>(vk);
    state[index] =
        down ? static_cast<BYTE>(state[index] | 0x80) : static_cast<BYTE>(state[index] & 0x7f);
    SetKeyboardState(state.data());
  }

  void ClearSystemModifiers() {
    SetDown(VK_CONTROL, false);
    SetDown(VK_LCONTROL, false);
    SetDown(VK_RCONTROL, false);
    SetDown(VK_MENU, false);
    SetDown(VK_LMENU, false);
    SetDown(VK_RMENU, false);
    SetDown(VK_LWIN, false);
    SetDown(VK_RWIN, false);
  }

 private:
  std::array<BYTE, 256> original_{};
};

bool CurrentKeyboardLayoutProduces(WPARAM virtual_key, bool shift, WCHAR expected) {
  std::array<BYTE, 256> keyboard_state{};
  if (!GetKeyboardState(keyboard_state.data())) return false;
  for (const int key : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT}) {
    keyboard_state[static_cast<size_t>(key)] = shift ? 0x80 : 0;
  }

  const HKL keyboard_layout = GetKeyboardLayout(0);
  const UINT scan_code =
      MapVirtualKeyExW(static_cast<UINT>(virtual_key), MAPVK_VK_TO_VSC, keyboard_layout);
  std::array<WCHAR, 4> translated{};
  constexpr UINT kDoNotChangeKeyboardState = 1u << 2;
  const int translated_count = ToUnicodeEx(
      static_cast<UINT>(virtual_key), scan_code, keyboard_state.data(), translated.data(),
      static_cast<int>(translated.size()), kDoNotChangeKeyboardState, keyboard_layout);
  return translated_count == 1 && translated[0] == expected;
}

std::optional<WCHAR> TranslateDefaultOemCompositionCharacterForTest(WPARAM virtual_key, LPARAM) {
  std::array<BYTE, 256> keyboard_state{};
  if (!GetKeyboardState(keyboard_state.data())) return std::nullopt;
  const bool shift_down = (keyboard_state[VK_SHIFT] & 0x80) != 0;
  switch (virtual_key) {
    case VK_OEM_COMMA:
      return shift_down ? L'<' : L',';
    case VK_OEM_PERIOD:
      return shift_down ? L'>' : L'.';
    case VK_OEM_2:
      return shift_down ? L'?' : L'/';
    default:
      return std::nullopt;
  }
}

class OemCompositionTranslationGuard {
 public:
  OemCompositionTranslationGuard() {
    azookey::tsf::testing::SetTranslateOemCompositionCharacterForTest(
        &TranslateDefaultOemCompositionCharacterForTest);
  }

  ~OemCompositionTranslationGuard() {
    azookey::tsf::testing::ClearTranslateOemCompositionCharacterForTest();
  }
};

std::optional<char> TranslateDefaultAsciiDecimalDigitForTest(WPARAM virtual_key, LPARAM) {
  if (virtual_key >= VK_NUMPAD0 && virtual_key <= VK_NUMPAD9) {
    return static_cast<char>('0' + (virtual_key - VK_NUMPAD0));
  }
  if (virtual_key < '0' || virtual_key > '9') return std::nullopt;

  std::array<BYTE, 256> keyboard_state{};
  if (!GetKeyboardState(keyboard_state.data())) return std::nullopt;
  const bool shift_down = (keyboard_state[VK_SHIFT] & 0x80) != 0 ||
                          (keyboard_state[VK_LSHIFT] & 0x80) != 0 ||
                          (keyboard_state[VK_RSHIFT] & 0x80) != 0;
  return shift_down ? std::nullopt : std::optional<char>(static_cast<char>(virtual_key));
}

class AsciiDecimalDigitTranslationGuard {
 public:
  AsciiDecimalDigitTranslationGuard() {
    azookey::tsf::testing::SetTranslateAsciiDecimalDigitForTest(
        &TranslateDefaultAsciiDecimalDigitForTest);
  }

  ~AsciiDecimalDigitTranslationGuard() {
    azookey::tsf::testing::ClearTranslateAsciiDecimalDigitForTest();
  }
};

BOOL WINAPI FakePhysicalCursorPosition(POINT* point) {
  if (!point) return FALSE;
  *point = {321, 654};
  return TRUE;
}

UINT FakeMonitorScalePercent(POINT) { return 100; }

thread_local HWND g_last_logical_to_physical_window = nullptr;
thread_local int g_logical_to_physical_count = 0;

BOOL WINAPI FakeLogicalToPhysicalPoint(HWND window, POINT* point) {
  if (!point) return FALSE;
  g_last_logical_to_physical_window = window;
  ++g_logical_to_physical_count;
  point->x += 100;
  point->y += 200;
  return TRUE;
}

class CaretFallbackGuard {
 public:
  CaretFallbackGuard() {
    ResetLogicalToPhysicalCalls();
    azookey::tsf::testing::SetCaretWin32ApiForTest(nullptr, nullptr, &FakePhysicalCursorPosition,
                                                   &FakeLogicalToPhysicalPoint,
                                                   &FakeMonitorScalePercent);
  }

  ~CaretFallbackGuard() { azookey::tsf::testing::ClearCaretWin32ApiForTest(); }

  void ResetLogicalToPhysicalCalls() {
    g_last_logical_to_physical_window = nullptr;
    g_logical_to_physical_count = 0;
  }

  HWND last_logical_to_physical_window() const { return g_last_logical_to_physical_window; }

  int logical_to_physical_count() const { return g_logical_to_physical_count; }
};

template <typename Predicate>
bool WaitUntil(Predicate predicate,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

class FakeRange : public ITfRange {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfRange) {
      *ppvObject = static_cast<ITfRange*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    return static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  }

  STDMETHODIMP GetText(TfEditCookie, DWORD, WCHAR*, ULONG, ULONG* text_length) override {
    if (text_length) *text_length = 0;
    return E_NOTIMPL;
  }

  STDMETHODIMP SetText(TfEditCookie, DWORD, const WCHAR* text, LONG length) override {
    ++set_text_count;
    if (FAILED(set_text_result)) return set_text_result;
    last_text.assign(text, text + length);
    return set_text_result;
  }

  STDMETHODIMP GetFormattedText(TfEditCookie, IDataObject** data_object) override {
    if (data_object) *data_object = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown** object) override {
    if (object) *object = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP InsertEmbedded(TfEditCookie, DWORD, IDataObject*) override { return E_NOTIMPL; }

  STDMETHODIMP ShiftStart(TfEditCookie, LONG, LONG* shifted, const TF_HALTCOND*) override {
    if (shifted) *shifted = 0;
    return E_NOTIMPL;
  }

  STDMETHODIMP ShiftEnd(TfEditCookie, LONG, LONG* shifted, const TF_HALTCOND*) override {
    if (shifted) *shifted = 0;
    return E_NOTIMPL;
  }

  STDMETHODIMP ShiftStartToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }

  STDMETHODIMP ShiftEndToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }

  STDMETHODIMP ShiftStartRegion(TfEditCookie, TfShiftDir, BOOL* no_region) override {
    if (no_region) *no_region = FALSE;
    return E_NOTIMPL;
  }

  STDMETHODIMP ShiftEndRegion(TfEditCookie, TfShiftDir, BOOL* no_region) override {
    if (no_region) *no_region = FALSE;
    return E_NOTIMPL;
  }

  STDMETHODIMP IsEmpty(TfEditCookie, BOOL* empty) override {
    if (!empty) return E_POINTER;
    *empty = last_text.empty() ? TRUE : FALSE;
    return S_OK;
  }

  STDMETHODIMP Collapse(TfEditCookie, TfAnchor anchor) override {
    ++collapse_count;
    last_anchor = anchor;
    return collapse_result;
  }

  STDMETHODIMP IsEqualStart(TfEditCookie, ITfRange*, TfAnchor, BOOL* equal) override {
    if (equal) *equal = FALSE;
    return E_NOTIMPL;
  }

  STDMETHODIMP IsEqualEnd(TfEditCookie, ITfRange*, TfAnchor, BOOL* equal) override {
    if (equal) *equal = FALSE;
    return E_NOTIMPL;
  }

  STDMETHODIMP CompareStart(TfEditCookie, ITfRange*, TfAnchor, LONG* result) override {
    if (result) *result = 0;
    return E_NOTIMPL;
  }

  STDMETHODIMP CompareEnd(TfEditCookie, ITfRange*, TfAnchor, LONG* result) override {
    if (result) *result = 0;
    return E_NOTIMPL;
  }

  STDMETHODIMP AdjustForInsert(TfEditCookie, ULONG, BOOL* insert_ok) override {
    if (insert_ok) *insert_ok = TRUE;
    return S_OK;
  }

  STDMETHODIMP GetGravity(TfGravity* start, TfGravity* end) override {
    if (!start || !end) return E_POINTER;
    *start = TF_GRAVITY_FORWARD;
    *end = TF_GRAVITY_FORWARD;
    return S_OK;
  }

  STDMETHODIMP SetGravity(TfEditCookie, TfGravity, TfGravity) override { return S_OK; }

  STDMETHODIMP Clone(ITfRange** clone) override {
    if (!clone) return E_POINTER;
    *clone = nullptr;
    if (FAILED(clone_result)) return clone_result;
    ITfRange* cloned_range = clone_range ? clone_range : static_cast<ITfRange*>(this);
    cloned_range->AddRef();
    *clone = cloned_range;
    return clone_result;
  }

  STDMETHODIMP GetContext(ITfContext** context) override {
    if (context) *context = nullptr;
    return E_NOTIMPL;
  }

  int set_text_count{0};
  int collapse_count{0};
  std::wstring last_text;
  TfAnchor last_anchor{TF_ANCHOR_START};
  HRESULT set_text_result{S_OK};
  HRESULT collapse_result{S_OK};
  HRESULT clone_result{S_OK};
  ITfRange* clone_range{nullptr};

 private:
  LONG ref_count_{1};
};

class FakeContextView final : public ITfContextView {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfContextView) {
      *ppvObject = static_cast<ITfContextView*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    return static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  }

  STDMETHODIMP GetRangeFromPoint(TfEditCookie, const POINT*, DWORD, ITfRange** range) override {
    if (range) *range = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetTextExt(TfEditCookie ec, ITfRange* range, RECT* rect, BOOL* clipped) override {
    ++get_text_ext_count;
    last_edit_cookie = ec;
    last_range = range;
    if (rect) *rect = text_ext;
    if (clipped) *clipped = text_clipped;
    return get_text_ext_result;
  }

  STDMETHODIMP GetScreenExt(RECT* rect) override {
    if (rect) *rect = {};
    return E_NOTIMPL;
  }

  STDMETHODIMP GetWnd(HWND* window) override {
    if (!window) return E_POINTER;
    ++get_wnd_count;
    *window = text_window;
    return get_wnd_result;
  }

  RECT text_ext{};
  BOOL text_clipped{FALSE};
  HRESULT get_text_ext_result{S_OK};
  HRESULT get_wnd_result{E_FAIL};
  HWND text_window{nullptr};
  int get_text_ext_count{0};
  int get_wnd_count{0};
  TfEditCookie last_edit_cookie{0};
  ITfRange* last_range{nullptr};

 private:
  LONG ref_count_{1};
};

class NoopContext : public ITfContext {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (riid == IID_IUnknown && identity_unknown_) {
      identity_unknown_->AddRef();
      *ppvObject = identity_unknown_;
      return S_OK;
    }
    if (riid == IID_IUnknown || riid == IID_ITfContext) {
      *ppvObject = static_cast<ITfContext*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    return static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  }

  STDMETHODIMP RequestEditSession(TfClientId tid, ITfEditSession* edit_session, DWORD flags,
                                  HRESULT* session_result) override {
    request_count++;
    last_client_id = tid;
    last_flags = flags;
    if (!session_result) return E_POINTER;
    if (FAILED(request_result)) {
      *session_result = request_session_result;
      return request_result;
    }
    if (run_edit_session && edit_session) {
      *session_result = edit_session->DoEditSession(edit_cookie);
      return request_result;
    }
    *session_result = request_session_result;
    return request_result;
  }

  STDMETHODIMP InWriteSession(TfClientId, BOOL* write_session) override {
    if (!write_session) return E_POINTER;
    *write_session = FALSE;
    return S_OK;
  }

  STDMETHODIMP GetSelection(TfEditCookie, ULONG, ULONG selection_count, TF_SELECTION* selection,
                            ULONG* fetched) override {
    if (!fetched) return E_POINTER;
    *fetched = 0;
    if (!selection_range || selection_count == 0 || !selection) return E_NOTIMPL;
    selection[0] = {};
    selection_range->AddRef();
    selection[0].range = selection_range;
    *fetched = 1;
    return S_OK;
  }

  STDMETHODIMP SetSelection(TfEditCookie, ULONG selection_count,
                            const TF_SELECTION* selection) override {
    ++set_selection_count;
    last_selection_count = selection_count;
    last_selection_range = selection_count > 0 && selection ? selection[0].range : nullptr;
    return set_selection_result;
  }

  STDMETHODIMP GetStart(TfEditCookie, ITfRange** start) override {
    if (start) *start = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetEnd(TfEditCookie, ITfRange** end) override {
    if (end) *end = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetActiveView(ITfContextView** view) override {
    if (!view) return E_POINTER;
    *view = nullptr;
    if (FAILED(get_active_view_result)) return get_active_view_result;
    if (!active_view) return E_NOTIMPL;
    active_view->AddRef();
    *view = active_view;
    return S_OK;
  }

  STDMETHODIMP EnumViews(IEnumTfContextViews** enum_views) override {
    if (enum_views) *enum_views = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetStatus(TF_STATUS* status) override {
    if (!status) return E_POINTER;
    *status = {};
    return S_OK;
  }

  STDMETHODIMP GetProperty(REFGUID, ITfProperty** property) override {
    if (property) *property = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetAppProperty(REFGUID, ITfReadOnlyProperty** property) override {
    if (property) *property = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP TrackProperties(const GUID**, ULONG, const GUID**, ULONG,
                               ITfReadOnlyProperty** property) override {
    if (property) *property = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP EnumProperties(IEnumTfProperties** enum_properties) override {
    if (enum_properties) *enum_properties = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr** document_mgr) override {
    if (!document_mgr) return E_POINTER;
    *document_mgr = nullptr;
    if (!document_mgr_) return E_NOTIMPL;
    document_mgr_->AddRef();
    *document_mgr = document_mgr_;
    return S_OK;
  }

  STDMETHODIMP CreateRangeBackup(TfEditCookie, ITfRange*, ITfRangeBackup** backup) override {
    if (backup) *backup = nullptr;
    return E_NOTIMPL;
  }

  int request_count{0};
  TfClientId last_client_id{TF_CLIENTID_NULL};
  DWORD last_flags{0};
  HRESULT request_result{S_OK};
  HRESULT request_session_result{S_OK};
  bool run_edit_session{false};
  TfEditCookie edit_cookie{1};
  ITfRange* selection_range{nullptr};
  ITfContextView* active_view{nullptr};
  HRESULT get_active_view_result{S_OK};
  ITfDocumentMgr* document_mgr_{nullptr};
  IUnknown* identity_unknown_{nullptr};
  int set_selection_count{0};
  ULONG last_selection_count{0};
  ITfRange* last_selection_range{nullptr};
  HRESULT set_selection_result{S_OK};

 private:
  LONG ref_count_{1};
};

class FakeDocumentMgr final : public ITfDocumentMgr {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (riid == IID_IUnknown && identity_unknown_) {
      identity_unknown_->AddRef();
      *ppvObject = identity_unknown_;
      return S_OK;
    }
    if (riid == IID_IUnknown || riid == IID_ITfDocumentMgr) {
      *ppvObject = static_cast<ITfDocumentMgr*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    return static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  }

  STDMETHODIMP CreateContext(TfClientId, DWORD, IUnknown*, ITfContext** context,
                             TfEditCookie* edit_cookie) override {
    if (context) *context = nullptr;
    if (edit_cookie) *edit_cookie = TF_INVALID_EDIT_COOKIE;
    return E_NOTIMPL;
  }

  STDMETHODIMP Push(ITfContext*) override { return E_NOTIMPL; }

  STDMETHODIMP Pop(DWORD) override { return E_NOTIMPL; }

  STDMETHODIMP GetTop(ITfContext** context) override { return ReturnContext(context); }

  STDMETHODIMP GetBase(ITfContext** context) override { return ReturnContext(context); }

  STDMETHODIMP EnumContexts(IEnumTfContexts** enum_contexts) override {
    if (enum_contexts) *enum_contexts = nullptr;
    return E_NOTIMPL;
  }

  ITfContext* context_{nullptr};
  IUnknown* identity_unknown_{nullptr};

 private:
  HRESULT ReturnContext(ITfContext** context) {
    if (!context) return E_POINTER;
    *context = nullptr;
    if (!context_) return E_NOTIMPL;
    context_->AddRef();
    *context = context_;
    return S_OK;
  }

  LONG ref_count_{1};
};

class FakeComposition : public ITfComposition {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfComposition) {
      *ppvObject = static_cast<ITfComposition*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    return static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  }

  STDMETHODIMP GetRange(ITfRange** range) override {
    if (!range) return E_POINTER;
    *range = nullptr;
    if (FAILED(get_range_result)) return get_range_result;
    if (!range_) return E_NOTIMPL;
    range_->AddRef();
    *range = range_;
    return S_OK;
  }

  STDMETHODIMP ShiftStart(TfEditCookie, ITfRange*) override { return E_NOTIMPL; }

  STDMETHODIMP ShiftEnd(TfEditCookie, ITfRange*) override { return E_NOTIMPL; }

  STDMETHODIMP EndComposition(TfEditCookie) override {
    ++end_count;
    return end_result;
  }

  int end_count{0};
  HRESULT end_result{S_OK};
  ITfRange* range_{nullptr};
  HRESULT get_range_result{S_OK};

 private:
  LONG ref_count_{1};
};

class TextServiceHarness {
 public:
  ~TextServiceHarness() { service.Deactivate(); }

  BOOL Press(WPARAM key) {
    BOOL eaten = FALSE;
    const HRESULT hr = service.OnKeyDown(&context, key, 0, &eaten);
    EXPECT_TRUE(SUCCEEDED(hr)) << "OnKeyDown failed for key " << key;
    return eaten;
  }

  BOOL TestPress(WPARAM key) {
    BOOL eaten = FALSE;
    const HRESULT hr = service.OnTestKeyDown(&context, key, 0, &eaten);
    EXPECT_TRUE(SUCCEEDED(hr)) << "OnTestKeyDown failed for key " << key;
    return eaten;
  }

  KeyboardStateGuard keyboard_state;
  NoopContext context;
  azookey::tsf::TextService service;
};

class FakeCompositionAttachment {
 public:
  explicit FakeCompositionAttachment(TextServiceHarness& harness) : harness_(harness) {
    composition_range.clone_range = &selection_range;
    composition.AddRef();
    composition.range_ = &composition_range;
    harness_.service.composition_ = &composition;
    harness_.context.run_edit_session = true;
  }

  ~FakeCompositionAttachment() {
    if (harness_.service.composition_ == &composition) {
      harness_.service.composition_ = nullptr;
      composition.Release();
    }
  }

  FakeComposition composition;
  FakeRange composition_range;
  FakeRange selection_range;

 private:
  TextServiceHarness& harness_;
};

}  // namespace

namespace {
struct BracketDocument {
  std::wstring text;
  LONG start{0};
  LONG end{0};
  int writes{0};
  bool fail_read{false};
  bool fail_write{false};
  bool short_shift{false};
};

class DocumentRange final : public FakeRange {
 public:
  DocumentRange(std::shared_ptr<BracketDocument> doc, LONG first, LONG last)
      : document(std::move(doc)), start(first), end(last) {}
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = --references_;
    if (!remaining) delete this;
    return remaining;
  }
  STDMETHODIMP GetText(TfEditCookie, DWORD flags, WCHAR* text, ULONG capacity,
                       ULONG* count) override {
    *count = 0;
    if (document->fail_read) return E_FAIL;
    *count = std::min(capacity, static_cast<ULONG>(end - start));
    std::copy_n(document->text.data() + start, *count, text);
    if (flags & TF_TF_MOVESTART) start += static_cast<LONG>(*count);
    return S_OK;
  }
  STDMETHODIMP SetText(TfEditCookie, DWORD, const WCHAR* text, LONG length) override {
    if (document->fail_write) return E_FAIL;
    ++document->writes;
    document->text.replace(static_cast<size_t>(start), static_cast<size_t>(end - start), text,
                           static_cast<size_t>(length));
    end = start + length;
    return S_OK;
  }
  STDMETHODIMP IsEmpty(TfEditCookie, BOOL* empty) override {
    *empty = start == end;
    return S_OK;
  }
  STDMETHODIMP Collapse(TfEditCookie, TfAnchor anchor) override {
    if (anchor == TF_ANCHOR_START)
      end = start;
    else
      start = end;
    return S_OK;
  }
  STDMETHODIMP ShiftStart(TfEditCookie, LONG amount, LONG* shifted, const TF_HALTCOND*) override {
    const LONG next = document->short_shift ? start
                                            : std::clamp(start + amount, 0L,
                                                         static_cast<LONG>(document->text.size()));
    *shifted = next - start;
    start = next;
    end = std::max(start, end);
    return S_OK;
  }
  STDMETHODIMP ShiftEnd(TfEditCookie, LONG amount, LONG* shifted, const TF_HALTCOND*) override {
    const LONG next = document->short_shift
                          ? end
                          : std::clamp(end + amount, 0L, static_cast<LONG>(document->text.size()));
    *shifted = next - end;
    end = next;
    start = std::min(start, end);
    return S_OK;
  }
  STDMETHODIMP Clone(ITfRange** copy) override {
    *copy = new DocumentRange(document, start, end);
    return S_OK;
  }
  std::shared_ptr<BracketDocument> document;
  LONG start;
  LONG end;

 private:
  ULONG references_{1};
};

class DocumentComposition final : public FakeComposition {
 public:
  DocumentComposition(ITfRange* range, ITfCompositionSink* sink) : sink_(sink) {
    range_ = range;
    range_->AddRef();
    sink_->AddRef();
  }
  ~DocumentComposition() {
    range_->Release();
    sink_->Release();
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = --references_;
    if (!remaining) delete this;
    return remaining;
  }
  STDMETHODIMP EndComposition(TfEditCookie cookie) override {
    if (FAILED(end_result)) return end_result;
    return sink_->OnCompositionTerminated(cookie, this);
  }

 private:
  ITfCompositionSink* sink_;
  ULONG references_{1};
};

class DocumentContext final : public NoopContext, public ITfContextComposition {
 public:
  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    if (iid == IID_ITfContextComposition) {
      *object = static_cast<ITfContextComposition*>(this);
      AddRef();
      return S_OK;
    }
    return NoopContext::QueryInterface(iid, object);
  }
  STDMETHODIMP_(ULONG) AddRef() override { return NoopContext::AddRef(); }
  STDMETHODIMP_(ULONG) Release() override { return NoopContext::Release(); }
  STDMETHODIMP RequestEditSession(TfClientId id, ITfEditSession* session, DWORD flags,
                                  HRESULT* result) override {
    if ((flags & TF_ES_READWRITE) == TF_ES_READ && reject_read) {
      *result = TF_E_SYNCHRONOUS;
      return S_OK;
    }
    if ((flags & TF_ES_READWRITE) == TF_ES_READWRITE && reject_write) {
      *result = TS_E_READONLY;
      return S_OK;
    }
    run_edit_session = !skip_callback;
    return NoopContext::RequestEditSession(id, session, flags, result);
  }
  STDMETHODIMP GetSelection(TfEditCookie, ULONG, ULONG, TF_SELECTION* selection,
                            ULONG* fetched) override {
    selection->range = new DocumentRange(document, document->start, document->end);
    selection->style = {};
    *fetched = 1;
    return S_OK;
  }
  STDMETHODIMP SetSelection(TfEditCookie, ULONG, const TF_SELECTION* selection) override {
    if (FAILED(set_selection_result)) return set_selection_result;
    const auto* range = static_cast<const DocumentRange*>(selection->range);
    document->start = range->start;
    document->end = range->end;
    return S_OK;
  }
  STDMETHODIMP StartComposition(TfEditCookie, ITfRange* range, ITfCompositionSink* sink,
                                ITfComposition** composition) override {
    *composition = new DocumentComposition(range, sink);
    return S_OK;
  }
  STDMETHODIMP EnumCompositions(IEnumITfCompositionView**) override { return E_NOTIMPL; }
  STDMETHODIMP FindComposition(TfEditCookie, ITfRange*, IEnumITfCompositionView**) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP TakeOwnership(TfEditCookie, ITfCompositionView*, ITfCompositionSink*,
                             ITfComposition**) override {
    return E_NOTIMPL;
  }
  std::shared_ptr<BracketDocument> document = std::make_shared<BracketDocument>();
  bool reject_read{false};
  bool reject_write{false};
  bool skip_callback{false};
};

std::optional<WCHAR> TranslateBracketForTest(WPARAM key, LPARAM) {
  switch (key) {
    case VK_OEM_4:
      return L'[';
    case VK_OEM_6:
      return L']';
    case VK_OEM_7:
      return L'"';
    default:
      return std::nullopt;
  }
}

class BracketHarness {
 public:
  BracketHarness() {
    settings.pairing.enabled = true;
    service.set_foreground_app_for_test({"notepad.exe", "Notepad", true});
    service.set_bracket_settings_for_test(settings);
    azookey::tsf::testing::SetTranslateBracketCharacterForTest(&TranslateBracketForTest);
  }
  ~BracketHarness() {
    service.Deactivate();
    azookey::tsf::testing::ClearTranslateBracketCharacterForTest();
  }
  void Set(const std::wstring& text, LONG caret, LONG end = -1) {
    context.document->text = text;
    context.document->start = caret;
    context.document->end = end < 0 ? caret : end;
  }
  BOOL Press(WPARAM key, bool test = false, bool expect_success = true) {
    BOOL eaten = FALSE;
    const HRESULT hr = test ? service.OnTestKeyDown(&context, key, 0, &eaten)
                            : service.OnKeyDown(&context, key, 0, &eaten);
    if (expect_success) EXPECT_TRUE(SUCCEEDED(hr)) << std::hex << hr;
    return eaten;
  }
  void ApplySettings() { service.set_bracket_settings_for_test(settings); }
  KeyboardStateGuard keyboard;
  DocumentContext context;
  azookey::tsf::TextService service;
  azookey::core::BracketSettings settings;
};
}  // namespace

TEST(TsfTipBracketTest, AppPolicySuppressesBothKeyCallbacksAndReevaluatesSettings) {
  BracketHarness h;
  h.service.set_foreground_app_for_test({"CODE.EXE", "Chrome_WidgetWin_1", true});
  h.Press(VK_OEM_4, true);
  h.Press(VK_OEM_4);
  EXPECT_EQ(h.context.document->text.find(L"「」"), std::wstring::npos);
  h.Press(VK_ESCAPE);
  h.Set(L"", 0);
  h.settings = azookey::core::ParseBracketSettings(R"({"bracketPairing":true,
      "bracketPairingAppPolicy":"allowlist","bracketPairingApps":["code.exe"]})");
  h.ApplySettings();
  EXPECT_TRUE(h.Press(VK_OEM_4, true));
  EXPECT_TRUE(h.Press(VK_OEM_4));
  EXPECT_EQ(h.context.document->text, L"「」");
  h.Set(L"「」", 1);
  h.service.set_foreground_app_for_test({});
  EXPECT_FALSE(h.Press(VK_BACK, true));
  EXPECT_FALSE(h.Press(VK_BACK));
  EXPECT_EQ(h.context.document->text, L"「」");
}

TEST(TsfTipBracketTest, WindowsAppNamesCompareUnicodeWithoutLocaleDependence) {
  EXPECT_TRUE(azookey::tsf::WindowsAppNameEqual("ÉDITEUR.exe", "éditeur.EXE"));
  EXPECT_TRUE(azookey::tsf::WindowsAppNameEqual("日本語.exe", "日本語.EXE"));
  EXPECT_FALSE(azookey::tsf::WindowsAppNameEqual("Code.exe", "OtherCode.exe"));
  EXPECT_FALSE(azookey::tsf::WindowsAppNameEqual("\xff.exe", "\xff.EXE"));
}

TEST(TsfTipBracketTest, SymmetricQuotePairsSkipsAndInsertsWithinWords) {
  BracketHarness h;
  h.settings.pairing.symmetric_quote_pairing = true;
  h.ApplySettings();
  EXPECT_TRUE(h.Press(VK_OEM_7, true));
  EXPECT_TRUE(h.Press(VK_OEM_7));
  EXPECT_EQ(h.context.document->text, L"\"\"");
  EXPECT_EQ(h.context.document->start, 1);
  EXPECT_TRUE(h.Press(VK_OEM_7));
  EXPECT_EQ(h.context.document->text, L"\"\"");
  EXPECT_EQ(h.context.document->start, 2);
  h.Set(L"word", 4);
  EXPECT_TRUE(h.Press(VK_OEM_7));
  EXPECT_EQ(h.context.document->text, L"word\"");
  h.Set(L"", 0);
  h.context.reject_read = true;
  EXPECT_TRUE(h.Press(VK_OEM_7));
  EXPECT_EQ(h.context.document->text, L"\"");
}

TEST(TsfTipBracketTest, WrapSelectionWritesOncePreservesUnicodeAndPlacesCaretAfterClosing) {
  BracketHarness h;
  h.settings.pairing.wrap_selection = true;
  h.settings.trigger = azookey::core::BracketPairingTrigger::Composition;
  h.ApplySettings();
  h.Set(L"xあ😀y", 1, 4);
  EXPECT_TRUE(h.Press(VK_OEM_4, true));
  EXPECT_TRUE(h.Press(VK_OEM_4));
  EXPECT_EQ(h.context.document->text, L"x「あ😀」y");
  EXPECT_EQ(h.context.document->writes, 1);
  EXPECT_EQ(h.context.document->start, 6);
  EXPECT_EQ(h.context.document->end, 6);
  EXPECT_FALSE(h.service.bracket_composition_for_test());
  EXPECT_TRUE(h.service.queued_ipc_types_for_test().empty());
}

TEST(TsfTipBracketTest, WrapSelectionDoesNotWritePartialTextWhenReadFailsOrLimitIsExceeded) {
  BracketHarness h;
  h.settings.pairing.wrap_selection = true;
  h.ApplySettings();
  h.Set(L"selected", 0, 8);
  h.context.document->fail_read = true;
  EXPECT_FALSE(h.Press(VK_OEM_4, false, false));
  EXPECT_EQ(h.context.document->text, L"selected");
  h.context.document->fail_read = false;
  h.Set(std::wstring(65537, L'a'), 0, 65537);
  EXPECT_FALSE(h.Press(VK_OEM_4));
  EXPECT_EQ(h.context.document->text.size(), 65537u);
  EXPECT_EQ(h.context.document->writes, 0);
  h.Set(std::wstring(65536, L'a'), 0, 65536);
  EXPECT_TRUE(h.Press(VK_OEM_4));
  EXPECT_EQ(h.context.document->text.size(), 65538u);
  EXPECT_EQ(h.context.document->writes, 1);
}

TEST(TsfTipBracketTest, PendingPairFinishesInItsOwningContextAfterFocusMoves) {
  BracketHarness h;
  h.settings.trigger = azookey::core::BracketPairingTrigger::Composition;
  h.ApplySettings();
  ASSERT_TRUE(h.Press(VK_OEM_4));
  h.context.reject_write = true;
  EXPECT_EQ(h.service.OnSetFocus(FALSE), S_OK);
  ASSERT_TRUE(h.service.bracket_composition_for_test());
  h.context.reject_write = false;
  DocumentContext next;
  next.document->text = L"another document";
  BOOL eaten = FALSE;
  EXPECT_EQ(h.service.OnKeyDown(&next, VK_RETURN, 0, &eaten), S_OK);
  EXPECT_TRUE(eaten);
  EXPECT_FALSE(h.service.bracket_composition_for_test());
  EXPECT_EQ(h.context.document->start, 1);
  EXPECT_EQ(next.document->start, 0);
  EXPECT_EQ(next.document->text, L"another document");
}

TEST(TsfTipBracketTest, ImmediateInsertsSinglePairAndTypingPreservesClosingBracket) {
  BracketHarness h;
  EXPECT_TRUE(h.Press(VK_OEM_4, true));
  EXPECT_TRUE(h.Press(VK_OEM_4));
  EXPECT_EQ(h.context.document->text, L"「」");
  EXPECT_EQ(h.context.document->start, 1);
  EXPECT_EQ(h.context.document->end, 1);
  EXPECT_EQ(h.context.document->writes, 1);
  EXPECT_TRUE(h.service.queued_ipc_types_for_test().empty());
  EXPECT_FALSE(h.service.has_pending_ipc_query_for_test());
  EXPECT_TRUE(h.Press('A'));
  EXPECT_EQ(h.context.document->text, L"「あ」");
}

TEST(TsfTipBracketTest, SkipAndEmptyPairDeletionUseCurrentDocument) {
  BracketHarness h;
  h.Set(L"「あ」", 2);
  EXPECT_TRUE(h.Press(VK_OEM_6));
  EXPECT_EQ(h.context.document->text, L"「あ」");
  EXPECT_EQ(h.context.document->start, 3);
  EXPECT_EQ(h.context.document->writes, 0);
  h.Set(L"「」", 1);
  EXPECT_TRUE(h.Press(VK_BACK, true));
  EXPECT_TRUE(h.Press(VK_BACK));
  EXPECT_EQ(h.context.document->text, L"");
  EXPECT_EQ(h.context.document->start, 0);
  EXPECT_EQ(h.context.document->end, 0);
  EXPECT_EQ(h.context.document->writes, 1);
  h.Set(L"「あ」", 2);
  EXPECT_FALSE(h.Press(VK_BACK, true));
  EXPECT_FALSE(h.Press(VK_BACK));
}

TEST(TsfTipBracketTest, ReadRejectionFallsBackToLiteralAndPassesBackspace) {
  BracketHarness h;
  h.context.reject_read = true;
  EXPECT_TRUE(h.Press(VK_OEM_4));
  EXPECT_EQ(h.context.document->text, L"「");
  h.Set(L"「」", 1);
  EXPECT_FALSE(h.Press(VK_BACK, true));
  EXPECT_FALSE(h.Press(VK_BACK));
  EXPECT_TRUE(h.Press(VK_OEM_6));
  EXPECT_EQ(h.context.document->text, L"「」」");
}

TEST(TsfTipBracketTest, SelectionIsReplacedWithLiteralAndChangedHintCannotDelete) {
  BracketHarness h;
  h.Set(L"abc", 0, 3);
  EXPECT_TRUE(h.Press(VK_OEM_4));
  EXPECT_EQ(h.context.document->text, L"「");
  h.Set(L"「」", 1);
  EXPECT_TRUE(h.Press(VK_BACK, true));
  h.Set(L"「あ」", 2);
  EXPECT_FALSE(h.Press(VK_BACK));
  EXPECT_EQ(h.context.document->text, L"「あ」");
}

TEST(TsfTipBracketTest, CompositionConfirmsInsideOrCancelsWithoutIpc) {
  for (const bool cancel : {false, true}) {
    BracketHarness h;
    h.settings.trigger = azookey::core::BracketPairingTrigger::Composition;
    h.ApplySettings();
    EXPECT_TRUE(h.Press(VK_OEM_4));
    EXPECT_TRUE(h.service.bracket_composition_for_test());
    EXPECT_NE(h.service.composition_, nullptr);
    EXPECT_EQ(h.context.document->text, L"「」");
    EXPECT_EQ(h.context.document->start, 1);
    EXPECT_EQ(h.context.document->end, 1);
    EXPECT_TRUE(h.Press(cancel ? VK_ESCAPE : VK_RETURN, true));
    EXPECT_TRUE(h.Press(cancel ? VK_ESCAPE : VK_RETURN));
    EXPECT_EQ(h.context.document->text, cancel ? L"" : L"「」");
    EXPECT_EQ(h.context.document->start, cancel ? 0 : 1);
    EXPECT_EQ(h.context.document->end, h.context.document->start);
    EXPECT_FALSE(h.service.bracket_composition_for_test());
    EXPECT_EQ(h.service.composition_, nullptr);
    EXPECT_TRUE(h.service.queued_ipc_types_for_test().empty());
    EXPECT_FALSE(h.service.has_pending_ipc_query_for_test());
  }
}

TEST(TsfTipBracketTest, AlnumModesAndMasterOffRespectSettings) {
  BracketHarness h;
  h.settings.input_mode = azookey::core::BracketInputMode::AlnumHalf;
  h.ApplySettings();
  EXPECT_TRUE(h.Press(VK_OEM_4));
  EXPECT_EQ(h.context.document->text, L"[]");
  EXPECT_FALSE(h.Press('A', true));
  EXPECT_FALSE(h.Press('A'));
  h.Set(L"", 0);
  h.settings.input_mode = azookey::core::BracketInputMode::AlnumFull;
  h.ApplySettings();
  EXPECT_TRUE(h.Press(VK_OEM_4));
  EXPECT_EQ(h.context.document->text, L"［］");
  h.settings.pairing.enabled_in_alnum_mode = false;
  h.ApplySettings();
  EXPECT_FALSE(h.Press(VK_OEM_4, true));
  EXPECT_FALSE(h.Press(VK_OEM_4));
  h.settings.pairing.enabled = false;
  h.ApplySettings();
  EXPECT_FALSE(h.Press(VK_OEM_4, true));
  EXPECT_FALSE(h.Press(VK_OEM_4));
}

TEST(TsfTipBracketTest, PreeditCommitsBeforePairAndCompositionPairsNest) {
  BracketHarness h;
  EXPECT_TRUE(h.Press('A'));
  EXPECT_TRUE(h.Press(VK_OEM_4));
  EXPECT_EQ(h.context.document->text, L"あ「」");
  EXPECT_EQ(h.context.document->start, 2);
  h.Set(L"", 0);
  h.settings.trigger = azookey::core::BracketPairingTrigger::Composition;
  h.ApplySettings();
  EXPECT_TRUE(h.Press(VK_OEM_4));
  EXPECT_TRUE(h.Press(VK_OEM_4));
  EXPECT_EQ(h.context.document->text, L"「「」」");
  EXPECT_EQ(h.context.document->start, 2);
  EXPECT_TRUE(h.Press(VK_RETURN));
  EXPECT_EQ(h.context.document->start, 2);
}

TEST(TsfTipBracketTest, BatchAccumulationKeepsLiteralBracketsAndNoAutomaticPair) {
  BracketHarness h;
  h.service.set_batch_romaji_options_for_test(true, false, false);
  EXPECT_TRUE(h.Press('A'));
  EXPECT_TRUE(h.Press(VK_OEM_4, true));
  EXPECT_TRUE(h.Press(VK_OEM_4));
  EXPECT_EQ(h.context.document->text, L"あ[");
  EXPECT_FALSE(h.service.bracket_composition_for_test());
}

TEST(TsfTipBracketTest, SettingsReloadDoesNotDiscardPendingPairAndFocusLossCommitsIt) {
  BracketHarness h;
  h.settings.trigger = azookey::core::BracketPairingTrigger::Composition;
  h.ApplySettings();
  EXPECT_TRUE(h.Press(VK_OEM_4));
  h.settings.pairing.enabled = false;
  h.ApplySettings();
  EXPECT_TRUE(h.Press(VK_ESCAPE));
  EXPECT_EQ(h.context.document->text, L"");
  h.settings.pairing.enabled = true;
  h.ApplySettings();
  EXPECT_TRUE(h.Press(VK_OEM_4));
  EXPECT_TRUE(SUCCEEDED(h.service.OnSetFocus(FALSE)));
  EXPECT_EQ(h.context.document->text, L"「」");
  EXPECT_FALSE(h.service.bracket_composition_for_test());
  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_TRUE(h.service.queued_ipc_types_for_test().empty());
}

TEST(TsfTipBracketTest, RejectedWritesAndMissingCallbacksDoNotClaimInsertion) {
  BracketHarness h;
  h.context.reject_write = true;
  EXPECT_FALSE(h.Press(VK_OEM_4, false, false));
  EXPECT_EQ(h.context.document->text, L"");
  h.context.reject_write = false;
  h.context.skip_callback = true;
  EXPECT_FALSE(h.Press(VK_OEM_4, false, false));
  EXPECT_EQ(h.context.document->text, L"");
  h.context.skip_callback = false;
  h.context.set_selection_result = E_FAIL;
  EXPECT_TRUE(h.Press(VK_OEM_4));
  EXPECT_EQ(h.context.document->text, L"「」");
  EXPECT_EQ(h.context.document->writes, 1);
}

TEST(TsfTipOnKeyDownPreeditTest, TextServiceInstancesUseDistinctIpcClientIds) {
  azookey::tsf::TextService first;
  azookey::tsf::TextService second;

  EXPECT_FALSE(first.ipc_client_id_for_test().empty());
  EXPECT_FALSE(second.ipc_client_id_for_test().empty());
  EXPECT_NE(first.ipc_client_id_for_test(), second.ipc_client_id_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, OnTestKeyDownAllocationFailureReturnsOutOfMemory) {
  TextServiceHarness h;

  BOOL eaten = TRUE;
  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  EXPECT_EQ(h.service.OnTestKeyDown(&h.context, 'A', 0, &eaten), E_OUTOFMEMORY);
  EXPECT_EQ(eaten, FALSE);
}

TEST(TsfTipOnKeyDownPreeditTest, DoEditSessionAllocationFailureReturnsOutOfMemory) {
  NoopContext context;
  azookey::tsf::TextService service;
  auto* session = new azookey::tsf::EditSession(&service, &context);

  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  EXPECT_EQ(session->DoEditSession(1), E_OUTOFMEMORY);

  session->Release();
}

TEST(TsfTipOnKeyDownPreeditTest, PreeditUpdateAllocationFailureRollsBackTypedKey) {
  TextServiceHarness h;

  BOOL eaten = TRUE;
  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  EXPECT_EQ(h.service.OnKeyDown(&h.context, 'K', 0, &eaten), E_OUTOFMEMORY);
  EXPECT_EQ(eaten, FALSE);
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.context.request_count, 0);

  eaten = FALSE;
  EXPECT_EQ(h.service.OnKeyDown(&h.context, 'A', 0, &eaten), S_OK);
  EXPECT_EQ(eaten, TRUE);
  EXPECT_EQ(h.service.preedit_kana_, "あ");
}

TEST(TsfTipOnKeyDownPreeditTest, PreeditUpdateAllocationFailureKeepsActiveContext) {
  TextServiceHarness h;
  NoopContext next_context;

  ASSERT_TRUE(h.Press('K'));
  ASSERT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.active_context_is_for_test(&h.context));

  BOOL eaten = TRUE;
  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  EXPECT_EQ(h.service.OnKeyDown(&next_context, 'N', 0, &eaten), E_OUTOFMEMORY);
  EXPECT_EQ(eaten, FALSE);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_TRUE(h.service.active_context_is_for_test(&h.context));
  EXPECT_FALSE(h.service.active_context_is_for_test(&next_context));
  EXPECT_EQ(next_context.request_count, 0);
}

TEST(TsfTipOnKeyDownPreeditTest, PreeditUpdateAllocationFailureRestoresCandidateState) {
  TextServiceHarness h;

  ASSERT_TRUE(h.Press('K'));
  ASSERT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.Press(VK_SPACE));
  ASSERT_TRUE(h.service.candidate_window_show_pending_for_test());

  std::vector<azookey::ipc::CandidateField> candidates;
  azookey::ipc::CandidateField candidate;
  candidate.surface = "蚊";
  candidate.reading = "か";
  candidate.source = "test";
  candidates.push_back(candidate);
  h.service.set_cached_candidates_for_test(std::move(candidates));
  h.service.show_candidate_window_from_cache_for_test();
  ASSERT_FALSE(h.service.candidate_window_show_pending_for_test());
  ASSERT_EQ(h.service.shown_candidates_for_test().size(), 1u);

  BOOL eaten = TRUE;
  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  EXPECT_EQ(h.service.OnKeyDown(&h.context, 'N', 0, &eaten), E_OUTOFMEMORY);
  EXPECT_EQ(eaten, FALSE);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  ASSERT_EQ(h.service.shown_candidates_for_test().size(), 1u);
  EXPECT_EQ(h.service.shown_candidates_for_test()[0].surface, "蚊");

  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_FALSE(h.service.candidate_window_show_pending_for_test());
  ASSERT_EQ(h.service.shown_candidates_for_test().size(), 1u);
  EXPECT_EQ(h.service.shown_candidates_for_test()[0].surface, "蚊");
}

TEST(TsfTipOnKeyDownPreeditTest, PreeditUpdateAllocationFailureRollsBackBackspaceAndEscape) {
  TextServiceHarness h;

  ASSERT_TRUE(h.Press('K'));
  ASSERT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  BOOL eaten = TRUE;
  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  EXPECT_EQ(h.service.OnKeyDown(&h.context, VK_BACK, 0, &eaten), E_OUTOFMEMORY);
  EXPECT_EQ(eaten, FALSE);
  EXPECT_EQ(h.service.preedit_kana_, "か");

  eaten = TRUE;
  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  EXPECT_EQ(h.service.OnKeyDown(&h.context, VK_ESCAPE, 0, &eaten), E_OUTOFMEMORY);
  EXPECT_EQ(eaten, FALSE);
  EXPECT_EQ(h.service.preedit_kana_, "か");
}

TEST(TsfTipOnKeyDownPreeditTest, PreeditUpdateAllocationFailureRollsBackSpaceFlush) {
  TextServiceHarness h;

  ASSERT_TRUE(h.Press('K'));
  ASSERT_EQ(h.service.preedit_kana_, "");

  BOOL eaten = TRUE;
  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  EXPECT_EQ(h.service.OnKeyDown(&h.context, VK_SPACE, 0, &eaten), E_OUTOFMEMORY);
  EXPECT_EQ(eaten, FALSE);
  EXPECT_EQ(h.service.preedit_kana_, "");

  eaten = FALSE;
  EXPECT_EQ(h.service.OnKeyDown(&h.context, 'A', 0, &eaten), S_OK);
  EXPECT_EQ(eaten, TRUE);
  EXPECT_EQ(h.service.preedit_kana_, "か");
}

TEST(TsfTipOnKeyDownPreeditTest, CommitPreeditAllocationFailureReturnsOutOfMemory) {
  TextServiceHarness h;

  ASSERT_TRUE(h.Press('K'));
  ASSERT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  const int previous_request_count = h.context.request_count;

  BOOL eaten = TRUE;
  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  EXPECT_EQ(h.service.OnKeyDown(&h.context, VK_RETURN, 0, &eaten), E_OUTOFMEMORY);

  EXPECT_EQ(eaten, FALSE);
  EXPECT_EQ(h.context.request_count, previous_request_count);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_EQ(h.service.commit_surface_, "か");
  EXPECT_TRUE(h.service.committing_);
}

TEST(TsfTipOnKeyDownPreeditTest, CommitSelectedAllocationFailureReturnsOutOfMemory) {
  TextServiceHarness h;

  ASSERT_TRUE(h.Press('K'));
  ASSERT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  std::vector<azookey::ipc::CandidateField> candidates;
  azookey::ipc::CandidateField candidate;
  candidate.surface = "蚊";
  candidate.reading = "か";
  candidate.source = "test";
  candidates.push_back(candidate);
  h.service.set_cached_candidates_for_test(std::move(candidates));
  ASSERT_TRUE(h.Press(VK_SPACE));
  ASSERT_EQ(h.service.shown_candidates_for_test().size(), 1u);
  const int previous_request_count = h.context.request_count;

  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  EXPECT_EQ(h.service.commit_selected_for_test(&h.context), E_OUTOFMEMORY);

  EXPECT_EQ(h.context.request_count, previous_request_count);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_EQ(h.service.commit_surface_, "蚊");
  EXPECT_TRUE(h.service.committing_);
  EXPECT_TRUE(h.service.has_pending_commit_observation_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, AlphabetInputBuildsKanaPreeditAndEatsKeys) {
  TextServiceHarness h;

  EXPECT_TRUE(h.TestPress('K'));
  EXPECT_TRUE(h.Press('K'));
  EXPECT_EQ(h.service.preedit_kana_, "");

  EXPECT_TRUE(h.TestPress('A'));
  EXPECT_TRUE(h.Press('A'));
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_GE(h.context.request_count, 2);
  EXPECT_EQ(h.context.last_flags, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE);
}

TEST(TsfTipOnKeyDownPreeditTest, OemCompositionTranslationUsesCurrentKeyboardLayoutWhenSupported) {
  KeyboardStateGuard keyboard_state;
  keyboard_state.SetDown(VK_SHIFT, false);
  keyboard_state.SetDown(VK_LSHIFT, false);
  keyboard_state.SetDown(VK_RSHIFT, false);
  if (!CurrentKeyboardLayoutProduces(VK_OEM_COMMA, false, L',')) {
    GTEST_SKIP() << "requires a keyboard layout where unshifted VK_OEM_COMMA produces ','";
  }

  EXPECT_EQ(
      azookey::tsf::testing::TranslateOemCompositionCharacterUsingWin32ForTest(VK_OEM_COMMA, 0),
      std::optional<WCHAR>(L','));
}

TEST(TsfTipOnKeyDownPreeditTest, OemPunctuationAndSlashJoinActivePreedit) {
  TextServiceHarness h;
  OemCompositionTranslationGuard oem_translation;
  h.keyboard_state.SetDown(VK_SHIFT, false);
  h.keyboard_state.SetDown(VK_LSHIFT, false);
  h.keyboard_state.SetDown(VK_RSHIFT, false);

  EXPECT_FALSE(h.TestPress(VK_OEM_COMMA));
  EXPECT_FALSE(h.Press(VK_OEM_COMMA));
  EXPECT_FALSE(h.TestPress(VK_OEM_PERIOD));
  EXPECT_FALSE(h.Press(VK_OEM_PERIOD));
  EXPECT_FALSE(h.TestPress(VK_OEM_2));
  EXPECT_FALSE(h.Press(VK_OEM_2));

  FakeComposition composition;
  FakeRange range;
  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;

  EXPECT_TRUE(h.Press('N'));
  EXPECT_TRUE(h.TestPress(VK_OEM_COMMA));
  EXPECT_TRUE(h.Press(VK_OEM_COMMA));
  EXPECT_EQ(h.service.preedit_kana_, "ん、");
  EXPECT_EQ(range.last_text, L"ん、");

  EXPECT_TRUE(h.TestPress(VK_OEM_PERIOD));
  EXPECT_TRUE(h.Press(VK_OEM_PERIOD));
  EXPECT_EQ(h.service.preedit_kana_, "ん、。");
  EXPECT_EQ(range.last_text, L"ん、。");

  EXPECT_TRUE(h.TestPress(VK_OEM_2));
  EXPECT_TRUE(h.Press(VK_OEM_2));
  EXPECT_EQ(h.service.preedit_kana_, "ん、。/");
  EXPECT_EQ(range.last_text, L"ん、。/");

  EXPECT_TRUE(h.Press(VK_BACK));
  EXPECT_EQ(h.service.preedit_kana_, "ん、。");
  EXPECT_EQ(range.last_text, L"ん、。");

  h.keyboard_state.SetDown(VK_SHIFT, true);
  EXPECT_TRUE(h.TestPress(VK_OEM_2));
  EXPECT_TRUE(h.Press(VK_OEM_2));
  EXPECT_EQ(h.service.preedit_kana_, "ん、。?");
  EXPECT_EQ(range.last_text, L"ん、。?");

  EXPECT_TRUE(h.TestPress(VK_OEM_COMMA));
  EXPECT_TRUE(h.Press(VK_OEM_COMMA));
  EXPECT_EQ(h.service.preedit_kana_, "ん、。?<");
  EXPECT_EQ(range.last_text, L"ん、。?<");

  EXPECT_TRUE(h.TestPress(VK_OEM_PERIOD));
  EXPECT_TRUE(h.Press(VK_OEM_PERIOD));
  EXPECT_EQ(h.service.preedit_kana_, "ん、。?<>");
  EXPECT_EQ(range.last_text, L"ん、。?<>");
  h.keyboard_state.SetDown(VK_SHIFT, false);

  if (h.service.composition_) {
    h.service.composition_->Release();
    h.service.composition_ = nullptr;
  }
}

TEST(TsfTipOnKeyDownPreeditTest, BatchOemPunctuationKeepsAsciiRawAndBackspacesAtomically) {
  TextServiceHarness h;
  OemCompositionTranslationGuard oem_translation;
  h.service.set_batch_romaji_options_for_test(true);
  h.keyboard_state.SetDown(VK_SHIFT, false);
  h.keyboard_state.SetDown(VK_LSHIFT, false);
  h.keyboard_state.SetDown(VK_RSHIFT, false);

  EXPECT_TRUE(h.Press('N'));
  EXPECT_TRUE(h.Press('I'));
  EXPECT_TRUE(h.Press(VK_OEM_COMMA));
  EXPECT_EQ(h.service.preedit_kana_, "に、");

  EXPECT_TRUE(h.Press(VK_BACK));
  EXPECT_EQ(h.service.preedit_kana_, "に");

  EXPECT_TRUE(h.Press(VK_OEM_COMMA));
  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_EQ(h.service.pending_ipc_reading_for_test(), "に、");
  EXPECT_EQ(h.service.pending_ipc_raw_romaji_for_test(), "ni,");
}

TEST(TsfTipOnKeyDownPreeditTest, BatchRomajiAccumulatesKanaPreviewWithoutQuerying) {
  TextServiceHarness h;
  h.service.set_batch_romaji_options_for_test(true);

  EXPECT_TRUE(h.Press('N'));
  EXPECT_TRUE(h.Press('I'));
  EXPECT_TRUE(h.Press('H'));
  EXPECT_TRUE(h.Press('O'));
  EXPECT_TRUE(h.Press('N'));
  EXPECT_TRUE(h.Press('G'));
  EXPECT_TRUE(h.Press('O'));

  EXPECT_EQ(h.service.preedit_kana_, "にほんご");
  EXPECT_FALSE(h.service.has_pending_ipc_query_for_test());
  EXPECT_FALSE(h.service.candidate_window_show_pending_for_test());

  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_TRUE(h.service.has_pending_ipc_query_for_test());
  EXPECT_TRUE(h.service.pending_ipc_query_is_batch_for_test());
  EXPECT_EQ(h.service.pending_ipc_reading_for_test(), "にほんご");
  EXPECT_EQ(h.service.pending_ipc_raw_romaji_for_test(), "nihongo");
  EXPECT_TRUE(h.service.candidate_window_show_pending_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, BatchRomajiPreviewCanShowRawRomaji) {
  TextServiceHarness h;
  h.service.set_batch_romaji_options_for_test(true, true);

  EXPECT_TRUE(h.Press('N'));
  EXPECT_TRUE(h.Press('I'));

  EXPECT_EQ(h.service.preedit_kana_, "ni");
  EXPECT_FALSE(h.service.has_pending_ipc_query_for_test());

  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_EQ(h.service.pending_ipc_reading_for_test(), "に");
  EXPECT_EQ(h.service.pending_ipc_raw_romaji_for_test(), "ni");
}

TEST(TsfTipOnKeyDownPreeditTest, BatchConvertingEscapeCancelsAndKeepsAccumulation) {
  TextServiceHarness h;
  h.service.set_batch_romaji_options_for_test(true);

  EXPECT_TRUE(h.Press('N'));
  EXPECT_TRUE(h.Press('I'));
  EXPECT_EQ(h.service.preedit_kana_, "に");

  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_TRUE(h.service.has_pending_ipc_query_for_test());
  EXPECT_TRUE(h.service.pending_ipc_query_is_batch_for_test());
  EXPECT_TRUE(h.service.candidate_window_show_pending_for_test());

  EXPECT_TRUE(h.Press(VK_ESCAPE));
  EXPECT_EQ(h.service.preedit_kana_, "に");
  EXPECT_FALSE(h.service.has_pending_ipc_query_for_test());
  EXPECT_FALSE(h.service.candidate_window_show_pending_for_test());

  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_TRUE(h.service.has_pending_ipc_query_for_test());
  EXPECT_TRUE(h.service.pending_ipc_query_is_batch_for_test());
  EXPECT_EQ(h.service.pending_ipc_reading_for_test(), "に");
  EXPECT_EQ(h.service.pending_ipc_raw_romaji_for_test(), "ni");
}

TEST(TsfTipOnKeyDownPreeditTest, BatchConvertingSpaceDoesNotResendRequest) {
  TextServiceHarness h;
  h.service.set_batch_romaji_options_for_test(true);

  EXPECT_TRUE(h.Press('N'));
  EXPECT_TRUE(h.Press('I'));
  EXPECT_TRUE(h.Press(VK_SPACE));
  ASSERT_TRUE(h.service.has_pending_ipc_query_for_test());
  const uint64_t first_request_id = h.service.pending_ipc_request_id_for_test();

  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_TRUE(h.service.has_pending_ipc_query_for_test());
  EXPECT_EQ(h.service.pending_ipc_request_id_for_test(), first_request_id);
  EXPECT_EQ(h.service.pending_ipc_reading_for_test(), "に");
  EXPECT_EQ(h.service.pending_ipc_raw_romaji_for_test(), "ni");
}

TEST(TsfTipOnKeyDownPreeditTest, IpcResponseMatchingRequiresExpectedType) {
  azookey::ipc::Envelope response;
  response.version = 1;
  response.request_id = 4;
  response.trace_id = "stale-query";
  response.type = azookey::ipc::MessageType::QueryCandidates;

  EXPECT_FALSE(azookey::tsf::testing::IsExpectedIpcResponseForTest(
      response, 4, azookey::ipc::MessageType::CommitObservation));

  response.type = azookey::ipc::MessageType::CommitObservation;
  EXPECT_TRUE(azookey::tsf::testing::IsExpectedIpcResponseForTest(
      response, 4, azookey::ipc::MessageType::CommitObservation));

  response.request_id = 5;
  EXPECT_FALSE(azookey::tsf::testing::IsExpectedIpcResponseForTest(
      response, 4, azookey::ipc::MessageType::CommitObservation));
}

TEST(TsfTipOnKeyDownPreeditTest, HostGenerationChangeReissuesQueryRearmedAfterDisconnect) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-tip-host-generation-test-" + std::to_string(GetCurrentProcessId());
  std::atomic<bool> first_query_received{false};
  std::atomic<bool> replacement_handshake_received{false};
  std::atomic<bool> replacement_query_received{false};
  std::atomic<uint64_t> first_request_id{0};
  std::atomic<uint64_t> replacement_request_id{0};
  std::atomic<uint32_t> first_max_candidates{0};

  azookey::ipc::NamedPipeServer first_server;
  ASSERT_TRUE(first_server.Start(
      pipe_name, [&](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;
        if (req.type == azookey::ipc::MessageType::Handshake) {
          azookey::ipc::HandshakeResponse payload;
          payload.host_version = "test-host";
          payload.host_generation_id = "generation-a";
          payload.accepted = true;
          payload.max_candidates = 17;
          res.payload_json = azookey::ipc::BuildHandshakeResponse(payload);
          return res;
        }
        if (req.type == azookey::ipc::MessageType::QueryCandidates) {
          if (const auto payload = azookey::ipc::ParseQueryCandidatesRequest(req.payload_json)) {
            first_max_candidates.store(payload->max_candidates);
          }
          first_request_id.store(req.request_id);
          first_query_received.store(true);
        }
        return std::nullopt;
      }));

  TextServiceHarness h;
  h.service.set_ipc_pipe_name_for_test(pipe_name);
  ASSERT_TRUE(h.Press('K'));
  ASSERT_TRUE(h.Press('A'));
  h.service.start_ipc_worker_for_test();
  ASSERT_TRUE(WaitUntil([&] { return first_query_received.load(); }));
  EXPECT_EQ(first_max_candidates.load(), 17u);
  ASSERT_TRUE(h.Press(VK_SPACE));
  EXPECT_TRUE(h.service.candidate_window_show_pending_for_test());

  first_server.Stop();

  azookey::ipc::NamedPipeServer replacement_server;
  ASSERT_TRUE(replacement_server.Start(
      pipe_name, [&](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;
        if (req.type == azookey::ipc::MessageType::Handshake) {
          replacement_handshake_received.store(true);
          azookey::ipc::HandshakeResponse payload;
          payload.host_version = "test-host";
          payload.host_generation_id = "generation-b";
          payload.accepted = true;
          res.payload_json = azookey::ipc::BuildHandshakeResponse(payload);
          return res;
        }
        if (req.type == azookey::ipc::MessageType::QueryCandidates) {
          replacement_request_id.store(req.request_id);
          replacement_query_received.store(true);
          azookey::ipc::QueryCandidatesResponse payload;
          payload.candidates.push_back({"replacement", "か", 1.0, "test"});
          res.payload_json = azookey::ipc::BuildQueryCandidatesResponse(payload);
          return res;
        }
        return std::nullopt;
      }));

  ASSERT_TRUE(WaitUntil([&] { return replacement_handshake_received.load(); }));
  ASSERT_TRUE(WaitUntil([&] { return replacement_query_received.load(); }));
  ASSERT_TRUE(WaitUntil([&] { return !h.service.cached_candidates_for_test().empty(); }));
  EXPECT_NE(first_request_id.load(), replacement_request_id.load());
  EXPECT_EQ(h.service.cached_candidates_for_test().front().surface, "replacement");
  EXPECT_FALSE(h.service.has_pending_ipc_query_for_test());

  h.service.stop_ipc_worker_for_test();
  replacement_server.Stop();
}

// DEV-554: the send-queue bound has to hold while the host is unreachable, which
// is when the worker never reaches the code that drains the queue. Enqueueing
// past the cap must therefore drop the oldest observations right away.
TEST(TsfTipOnKeyDownPreeditTest, CommitObservationQueueIsBoundedWhileHostIsUnreachable) {
  TextServiceHarness h;
  ASSERT_FALSE(h.service.ipc_client_id_for_test().empty());

  // No IPC worker is started, so nothing ever drains the queue: this is the
  // long-outage shape, where the worker sits in the reconnect backoff.
  const size_t over_cap = azookey::ipc::kMaxQueuedCommitObservations + 8;
  for (size_t i = 0; i < over_cap; ++i) {
    azookey::ipc::CandidateField chosen;
    chosen.surface = "仮名" + std::to_string(i);
    chosen.reading = "かな";
    chosen.source = "test";
    h.service.post_commit_observation_for_test("かな", chosen);
  }

  EXPECT_EQ(h.service.queued_ipc_types_for_test().size(),
            azookey::ipc::kMaxQueuedCommitObservations);

  // The oldest are the ones dropped, so the newest commit is still queued and
  // the surviving head is the first observation past the overflow.
  const auto newest = h.service.last_queued_commit_observation_for_test();
  ASSERT_TRUE(newest.has_value());
  EXPECT_EQ(newest->chosen.surface, "仮名" + std::to_string(over_cap - 1));
  const auto oldest = h.service.first_queued_commit_observation_for_test();
  ASSERT_TRUE(oldest.has_value());
  EXPECT_EQ(oldest->chosen.surface,
            "仮名" + std::to_string(over_cap - azookey::ipc::kMaxQueuedCommitObservations));
}

// DEV-554: a host that dies before ACKing a CommitObservation used to lose that
// commit's learning telemetry, because the worker dropped the ACK-awaiting item
// when it left the connection loop. The item must survive the reconnect and be
// resent with the same observation_id so the host can dedupe it.
TEST(TsfTipOnKeyDownPreeditTest, CommitObservationIsResentAfterHostDiesBeforeAck) {
  const std::string pipe_name = "\\\\.\\pipe\\azookey-tip-commit-observation-resend-test-" +
                                std::to_string(GetCurrentProcessId());
  std::mutex observed_mtx;
  std::string first_observation_id;
  std::string second_observation_id;
  std::atomic<bool> first_commit_received{false};
  std::atomic<bool> second_commit_received{false};

  TextServiceHarness h;
  h.service.set_ipc_pipe_name_for_test(pipe_name);
  ASSERT_FALSE(h.service.ipc_client_id_for_test().empty());

  // Queue the observation before any worker runs, so the send queue holds
  // exactly the commit traffic this test cares about.
  h.service.preedit_kana_ = "かな";
  std::vector<azookey::ipc::CandidateField> host_candidates(1);
  host_candidates[0].surface = "仮名";
  host_candidates[0].reading = "かな";
  host_candidates[0].source = "test";
  h.service.set_rewritten_cached_candidates_for_test("かな", std::move(host_candidates));
  ASSERT_TRUE(h.Press(VK_SPACE));
  h.service.set_selected_candidate_index_for_test(0);
  {
    FakeCompositionAttachment attachment(h);
    ASSERT_EQ(h.service.commit_selected_for_test(&h.context), S_OK);
  }
  const auto queued = h.service.last_queued_commit_observation_for_test();
  ASSERT_TRUE(queued.has_value());
  ASSERT_FALSE(queued->observation_id.empty());

  // First host: accepts the handshake, receives the observation, then dies
  // without replying (kill-before-ACK).
  azookey::ipc::NamedPipeServer first_server;
  ASSERT_TRUE(first_server.Start(
      pipe_name, [&](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;
        if (req.type == azookey::ipc::MessageType::Handshake) {
          azookey::ipc::HandshakeResponse payload;
          payload.host_version = "test-host";
          payload.host_generation_id = "generation-a";
          payload.accepted = true;
          res.payload_json = azookey::ipc::BuildHandshakeResponse(payload);
          return res;
        }
        if (req.type == azookey::ipc::MessageType::CommitObservation) {
          if (const auto payload = azookey::ipc::ParseCommitObservationRequest(req.payload_json)) {
            std::lock_guard<std::mutex> lock(observed_mtx);
            first_observation_id = payload->observation_id;
          }
          first_commit_received.store(true);
        }
        return std::nullopt;
      }));

  h.service.start_ipc_worker_for_test();
  // A queued Cancel is drained first and can spend its own connect/handshake
  // timeouts before the observation reaches the pipe, so allow more than the
  // default wait.
  ASSERT_TRUE(
      WaitUntil([&] { return first_commit_received.load(); }, std::chrono::milliseconds(5000)));
  first_server.Stop();

  // Second host on the same per-user pipe must see the observation again.
  azookey::ipc::NamedPipeServer second_server;
  ASSERT_TRUE(second_server.Start(
      pipe_name, [&](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;
        if (req.type == azookey::ipc::MessageType::Handshake) {
          azookey::ipc::HandshakeResponse payload;
          payload.host_version = "test-host";
          payload.host_generation_id = "generation-b";
          payload.accepted = true;
          res.payload_json = azookey::ipc::BuildHandshakeResponse(payload);
          return res;
        }
        if (req.type == azookey::ipc::MessageType::CommitObservation) {
          if (const auto payload = azookey::ipc::ParseCommitObservationRequest(req.payload_json)) {
            std::lock_guard<std::mutex> lock(observed_mtx);
            second_observation_id = payload->observation_id;
          }
          second_commit_received.store(true);
          azookey::ipc::CommitObservationResponse ack;
          ack.ok = true;
          res.payload_json = azookey::ipc::BuildCommitObservationResponse(ack);
          return res;
        }
        return std::nullopt;
      }));

  // The worker waits up to 3s for the missing ACK before it gives up on the
  // dead connection, then reconnects with backoff.
  ASSERT_TRUE(
      WaitUntil([&] { return second_commit_received.load(); }, std::chrono::milliseconds(20000)));

  {
    std::lock_guard<std::mutex> lock(observed_mtx);
    EXPECT_EQ(first_observation_id, queued->observation_id);
    // The same key on both connections is what lets the host apply the commit
    // exactly once across the restart.
    EXPECT_EQ(second_observation_id, queued->observation_id);
  }

  h.service.stop_ipc_worker_for_test();
  second_server.Stop();
}

TEST(TsfTipOnKeyDownPreeditTest, HostGenerationChangeReissuesBatchQueryAndUnblocksSpace) {
  const std::string pipe_name = "\\\\.\\pipe\\azookey-tip-host-generation-batch-test-" +
                                std::to_string(GetCurrentProcessId());
  std::atomic<bool> first_handshake_received{false};
  std::atomic<bool> first_query_received{false};
  std::atomic<bool> replacement_query_received{false};
  std::atomic<uint64_t> first_request_id{0};
  std::atomic<uint64_t> replacement_request_id{0};
  std::atomic<uint32_t> first_max_candidates{0};

  azookey::ipc::NamedPipeServer first_server;
  ASSERT_TRUE(first_server.Start(
      pipe_name, [&](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;
        if (req.type == azookey::ipc::MessageType::Handshake) {
          first_handshake_received.store(true);
          azookey::ipc::HandshakeResponse payload;
          payload.host_version = "test-host";
          payload.host_generation_id = "generation-a";
          payload.accepted = true;
          payload.batch_romaji_conversion = true;
          payload.max_candidates = 23;
          res.payload_json = azookey::ipc::BuildHandshakeResponse(payload);
          return res;
        }
        if (req.type == azookey::ipc::MessageType::QueryBatchConversion) {
          if (const auto payload =
                  azookey::ipc::ParseQueryBatchConversionRequest(req.payload_json)) {
            first_max_candidates.store(payload->max_candidates);
          }
          first_request_id.store(req.request_id);
          first_query_received.store(true);
        }
        return std::nullopt;
      }));

  TextServiceHarness h;
  h.service.set_batch_romaji_options_for_test(true);
  h.service.set_ipc_pipe_name_for_test(pipe_name);
  h.service.start_ipc_worker_for_test();
  ASSERT_TRUE(WaitUntil([&] { return first_handshake_received.load(); }));
  ASSERT_TRUE(h.Press('K'));
  ASSERT_TRUE(h.Press('A'));
  ASSERT_TRUE(h.Press(VK_SPACE));
  EXPECT_TRUE(h.service.batch_query_in_progress_for_test());
  ASSERT_TRUE(WaitUntil([&] { return first_query_received.load(); }));
  EXPECT_EQ(first_max_candidates.load(), 23u);

  first_server.Stop();

  azookey::ipc::NamedPipeServer replacement_server;
  ASSERT_TRUE(replacement_server.Start(
      pipe_name, [&](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;
        if (req.type == azookey::ipc::MessageType::Handshake) {
          azookey::ipc::HandshakeResponse payload;
          payload.host_version = "test-host";
          payload.host_generation_id = "generation-b";
          payload.accepted = true;
          payload.batch_romaji_conversion = true;
          res.payload_json = azookey::ipc::BuildHandshakeResponse(payload);
          return res;
        }
        if (req.type == azookey::ipc::MessageType::QueryBatchConversion) {
          replacement_request_id.store(req.request_id);
          replacement_query_received.store(true);
          azookey::ipc::QueryBatchConversionResponse payload;
          payload.full_surface = "か";
          res.payload_json = azookey::ipc::BuildQueryBatchConversionResponse(payload);
          return res;
        }
        return std::nullopt;
      }));

  ASSERT_TRUE(WaitUntil([&] { return replacement_query_received.load(); }));
  ASSERT_TRUE(WaitUntil([&] { return !h.service.cached_candidates_for_test().empty(); }));
  EXPECT_NE(first_request_id.load(), replacement_request_id.load());
  h.service.show_candidate_window_from_cache_for_test();
  EXPECT_FALSE(h.service.batch_query_in_progress_for_test());

  h.service.stop_ipc_worker_for_test();
  replacement_server.Stop();
}

TEST(TsfTipOnKeyDownPreeditTest, QueryCandidatesTimeoutSendsCancelAndUsesFallback) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-tip-qc-timeout-test-" + std::to_string(GetCurrentProcessId());

  std::atomic<bool> saw_query{false};
  std::atomic<bool> saw_cancel{false};
  std::atomic<uint64_t> query_request_id{0};
  std::atomic<uint64_t> cancel_target_id{0};

  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name, [&](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;

        if (req.type == azookey::ipc::MessageType::Handshake) {
          azookey::ipc::HandshakeResponse payload;
          payload.host_version = "test-host";
          payload.protocol_version = 1;
          payload.accepted = true;
          res.payload_json = azookey::ipc::BuildHandshakeResponse(payload);
          return res;
        }

        if (req.type == azookey::ipc::MessageType::QueryCandidates) {
          saw_query.store(true);
          query_request_id.store(req.request_id);
          return std::nullopt;
        }

        if (req.type == azookey::ipc::MessageType::Cancel) {
          auto payload = azookey::ipc::ParseCancel(req.payload_json);
          if (payload) cancel_target_id.store(payload->target_request_id);
          saw_cancel.store(true);
          return std::nullopt;
        }

        return std::nullopt;
      });
  ASSERT_TRUE(started);

  TextServiceHarness h;
  h.service.set_ipc_pipe_name_for_test(pipe_name);

  ASSERT_TRUE(h.Press('K'));
  ASSERT_TRUE(h.Press('A'));
  ASSERT_TRUE(h.Press(VK_SPACE));
  ASSERT_TRUE(h.service.candidate_window_show_pending_for_test());

  h.service.start_ipc_worker_for_test();

  ASSERT_TRUE(WaitUntil([&] { return saw_query.load(); }));
  ASSERT_TRUE(WaitUntil([&] { return saw_cancel.load(); }));
  EXPECT_EQ(cancel_target_id.load(), query_request_id.load());

  ASSERT_TRUE(WaitUntil([&] { return !h.service.cached_candidates_for_test().empty(); }));
  auto cached = h.service.cached_candidates_for_test();
  ASSERT_EQ(cached.size(), 1u);
  EXPECT_EQ(cached[0].surface, "か");
  EXPECT_EQ(cached[0].reading, "か");
  EXPECT_EQ(cached[0].source, "fallback");

  h.service.show_candidate_window_from_cache_for_test();
  ASSERT_EQ(h.service.shown_candidates_for_test().size(), 1u);
  EXPECT_EQ(h.service.shown_candidates_for_test()[0].surface, "か");
  EXPECT_FALSE(h.service.candidate_window_show_pending_for_test());

  h.service.stop_ipc_worker_for_test();
  server.Stop();
}

TEST(TsfTipOnKeyDownPreeditTest, InFlightQueryCancelReachesHostBeforeQueryReturns) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-tip-inflight-cancel-test-" + std::to_string(GetCurrentProcessId());

  std::atomic<bool> saw_query{false};
  std::atomic<bool> saw_cancel{false};
  std::atomic<bool> allow_query_return{false};
  std::atomic<bool> query_returned{false};
  std::atomic<bool> cancel_before_query_returned{false};
  std::atomic<uint64_t> query_request_id{0};
  std::atomic<uint64_t> cancel_target_id{0};

  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name, [&](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;

        if (req.type == azookey::ipc::MessageType::Handshake) {
          azookey::ipc::HandshakeResponse payload;
          payload.host_version = "test-host";
          payload.protocol_version = 1;
          payload.accepted = true;
          res.payload_json = azookey::ipc::BuildHandshakeResponse(payload);
          return res;
        }

        if (req.type == azookey::ipc::MessageType::QueryCandidates) {
          saw_query.store(true);
          query_request_id.store(req.request_id);
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
          while (!allow_query_return.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
          query_returned.store(true);
          azookey::ipc::QueryCandidatesResponse payload;
          azookey::ipc::CandidateField candidate;
          candidate.surface = "蚊";
          candidate.reading = "か";
          candidate.source = "test";
          payload.candidates.push_back(std::move(candidate));
          res.payload_json = azookey::ipc::BuildQueryCandidatesResponse(payload);
          return res;
        }

        if (req.type == azookey::ipc::MessageType::Cancel) {
          auto payload = azookey::ipc::ParseCancel(req.payload_json);
          if (payload) cancel_target_id.store(payload->target_request_id);
          cancel_before_query_returned.store(!query_returned.load());
          saw_cancel.store(true);
          allow_query_return.store(true);
          return std::nullopt;
        }

        return std::nullopt;
      });
  ASSERT_TRUE(started);

  TextServiceHarness h;
  h.service.set_ipc_pipe_name_for_test(pipe_name);

  ASSERT_TRUE(h.Press('K'));
  ASSERT_TRUE(h.Press('A'));
  h.service.start_ipc_worker_for_test();

  ASSERT_TRUE(WaitUntil([&] { return saw_query.load(); }));
  ASSERT_TRUE(h.Press(VK_RETURN));
  ASSERT_TRUE(WaitUntil([&] { return saw_cancel.load(); }));

  EXPECT_EQ(cancel_target_id.load(), query_request_id.load());
  EXPECT_TRUE(cancel_before_query_returned.load());

  allow_query_return.store(true);
  h.service.stop_ipc_worker_for_test();
  server.Stop();
}

TEST(TsfTipOnKeyDownPreeditTest, BatchRawRomajiPreviewCommitsKanaReadingAsIs) {
  TextServiceHarness h;
  FakeRange range;
  h.service.set_batch_romaji_options_for_test(true, true);

  EXPECT_TRUE(h.Press('N'));
  EXPECT_TRUE(h.Press('I'));
  EXPECT_EQ(h.service.preedit_kana_, "ni");

  h.context.selection_range = &range;
  h.context.run_edit_session = true;

  EXPECT_TRUE(h.Press(VK_RETURN));
  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x306b'));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.service.commit_surface_, "");
  EXPECT_FALSE(h.service.committing_);
}

TEST(TsfTipOnKeyDownPreeditTest, PendingNIsShownInPreeditWithoutCommittingRomaji) {
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange range;
  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;

  EXPECT_TRUE(h.Press('N'));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x3093'));

  EXPECT_TRUE(h.Press('A'));
  EXPECT_EQ(h.service.preedit_kana_, "な");
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x306a'));

  h.service.composition_->Release();
  h.service.composition_ = nullptr;
}

TEST(TsfTipOnKeyDownPreeditTest, PreeditUpdateMovesCaretWithASeparateRange) {
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange composition_range;
  FakeRange selection_range;
  composition_range.clone_range = &selection_range;
  composition.AddRef();
  composition.range_ = &composition_range;
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  EXPECT_TRUE(h.Press('N'));
  EXPECT_TRUE(h.Press('A'));

  EXPECT_EQ(composition_range.last_text, L"かな");
  EXPECT_TRUE(h.TestPress(VK_BACK));
  EXPECT_TRUE(h.Press(VK_BACK));

  EXPECT_EQ(composition_range.last_text, L"か");
  EXPECT_EQ(composition_range.collapse_count, 0);
  EXPECT_EQ(selection_range.collapse_count, 5);
  EXPECT_EQ(selection_range.last_anchor, TF_ANCHOR_END);
  EXPECT_EQ(h.context.set_selection_count, 5);
  EXPECT_EQ(h.context.last_selection_count, 1u);
  EXPECT_EQ(h.context.last_selection_range, &selection_range);

  h.service.composition_->Release();
  h.service.composition_ = nullptr;
}

TEST(TsfTipOnKeyDownPreeditTest, PreeditCaretUsesGetTextExtContractMatrix) {
  struct TestCase {
    const char* name;
    HRESULT get_text_ext_result;
    RECT text_ext;
    BOOL clipped;
    POINT expected_point;
    bool expect_view_transform;
  };
  const TestCase cases[] = {
      {"valid", S_OK, {10, 20, 30, 44}, FALSE, {110, 244}, true},
      // Current policy: a clipped but non-empty TSF extent remains a usable anchor.
      {"clipped", S_OK, {11, 21, 31, 45}, TRUE, {111, 245}, true},
      {"zero", S_OK, {}, FALSE, {321, 670}, false},
      {"no-layout", TS_E_NOLAYOUT, {10, 20, 30, 44}, FALSE, {321, 670}, false},
      {"no-lock", TF_E_NOLOCK, {10, 20, 30, 44}, FALSE, {321, 670}, false},
      {"failure", E_FAIL, {10, 20, 30, 44}, FALSE, {321, 670}, false},
  };

  CaretFallbackGuard caret_fallback;
  const HWND view_window = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(0x1234));
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.name);
    caret_fallback.ResetLogicalToPhysicalCalls();
    FakeContextView context_view;
    TextServiceHarness h;
    FakeCompositionAttachment attachment(h);
    h.context.active_view = &context_view;
    context_view.get_text_ext_result = test_case.get_text_ext_result;
    context_view.text_ext = test_case.text_ext;
    context_view.text_clipped = test_case.clipped;
    context_view.get_wnd_result = S_OK;
    context_view.text_window = view_window;

    EXPECT_TRUE(h.Press('K'));

    EXPECT_EQ(context_view.get_text_ext_count, 1);
    EXPECT_EQ(context_view.get_wnd_count, test_case.get_text_ext_result == S_OK ? 1 : 0);
    EXPECT_EQ(context_view.last_edit_cookie, h.context.edit_cookie);
    EXPECT_EQ(context_view.last_range, &attachment.composition_range);
    EXPECT_EQ(caret_fallback.logical_to_physical_count(), test_case.expect_view_transform ? 1 : 0);
    EXPECT_EQ(caret_fallback.last_logical_to_physical_window(),
              test_case.expect_view_transform ? view_window : nullptr);
    EXPECT_EQ(h.service.caret_point_valid_for_test(), true);
    EXPECT_EQ(h.service.caret_point_for_test().x, test_case.expected_point.x);
    EXPECT_EQ(h.service.caret_point_for_test().y, test_case.expected_point.y);

    h.context.active_view = nullptr;
  }
}

TEST(TsfTipOnKeyDownPreeditTest, RequestPreeditUpdateKeepsOuterAndSessionResultsIndependent) {
  struct TestCase {
    const char* name;
    HRESULT request_result;
    HRESULT session_result;
    HRESULT expected_result;
    bool expected_accepted;
  };
  const TestCase cases[] = {
      {"async", S_OK, TF_S_ASYNC, TF_S_ASYNC, true},
      {"read-only", S_OK, TS_E_READONLY, TS_E_READONLY, true},
      {"outer-failure", TF_E_LOCKED, TF_S_ASYNC, TF_E_LOCKED, false},
  };

  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.name);
    TextServiceHarness h;
    h.context.request_result = test_case.request_result;
    h.context.request_session_result = test_case.session_result;
    bool accepted = !test_case.expected_accepted;

    EXPECT_EQ(h.service.RequestPreeditUpdate(&h.context, &accepted), test_case.expected_result);
    EXPECT_EQ(accepted, test_case.expected_accepted);
    EXPECT_EQ(h.context.last_flags, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE);
  }
}

TEST(TsfTipOnKeyDownPreeditTest, RequestCommitEditSessionPropagatesRejectedSessionResults) {
  for (const HRESULT session_result : {TS_E_READONLY, TF_E_SYNCHRONOUS}) {
    SCOPED_TRACE(session_result);
    TextServiceHarness h;
    h.context.request_session_result = session_result;

    EXPECT_EQ(h.service.request_commit_edit_session_for_test(&h.context), session_result);
    EXPECT_EQ(h.context.last_flags, TF_ES_SYNC | TF_ES_READWRITE);
  }
}

TEST(TsfTipOnKeyDownPreeditTest, BackspaceClearsPendingNPreeditPreview) {
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange range;
  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;

  EXPECT_TRUE(h.Press('N'));
  ASSERT_EQ(range.last_text, std::wstring(1, L'\x3093'));

  EXPECT_TRUE(h.TestPress(VK_BACK));
  EXPECT_TRUE(h.Press(VK_BACK));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(composition.end_count, 1);
  EXPECT_EQ(range.last_text, std::wstring());
}

TEST(TsfTipOnKeyDownPreeditTest, BackspaceClearsPendingNnPreeditPreview) {
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange range;
  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;

  EXPECT_TRUE(h.Press('N'));
  EXPECT_TRUE(h.Press('N'));
  ASSERT_EQ(h.service.preedit_kana_, "");
  ASSERT_EQ(range.last_text, std::wstring(1, L'\x3093'));

  EXPECT_TRUE(h.TestPress(VK_BACK));
  EXPECT_TRUE(h.Press(VK_BACK));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(composition.end_count, 1);
  EXPECT_EQ(range.last_text, std::wstring());
}

TEST(TsfTipOnKeyDownPreeditTest, EscapeClearsPendingNPreeditPreviewText) {
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange range;
  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;

  EXPECT_TRUE(h.Press('N'));
  ASSERT_EQ(range.last_text, std::wstring(1, L'\x3093'));

  EXPECT_TRUE(h.TestPress(VK_ESCAPE));
  EXPECT_TRUE(h.Press(VK_ESCAPE));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(composition.end_count, 1);
  EXPECT_EQ(range.last_text, std::wstring());
}

TEST(TsfTipOnKeyDownPreeditTest, BackspaceRemovesPendingRomajiBeforeKana) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.TestPress(VK_BACK));
  EXPECT_TRUE(h.Press(VK_BACK));
  EXPECT_EQ(h.service.preedit_kana_, "");

  EXPECT_TRUE(h.Press('A'));
  EXPECT_EQ(h.service.preedit_kana_, "あ");
}

TEST(TsfTipOnKeyDownPreeditTest, BackspaceDeletesOneUtf8KanaFromPreedit) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  EXPECT_TRUE(h.Press('N'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "かな");

  EXPECT_TRUE(h.Press(VK_BACK));
  EXPECT_EQ(h.service.preedit_kana_, "か");

  EXPECT_TRUE(h.Press(VK_BACK));
  EXPECT_EQ(h.service.preedit_kana_, "");

  EXPECT_FALSE(h.TestPress(VK_BACK));
  EXPECT_FALSE(h.Press(VK_BACK));
}

TEST(TsfTipOnKeyDownPreeditTest, EscapeClearsPreeditAndPendingRomaji) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  EXPECT_TRUE(h.TestPress(VK_ESCAPE));
  EXPECT_TRUE(h.Press(VK_ESCAPE));
  EXPECT_EQ(h.service.preedit_kana_, "");

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press(VK_ESCAPE));
  EXPECT_EQ(h.service.preedit_kana_, "");

  EXPECT_TRUE(h.Press('A'));
  EXPECT_EQ(h.service.preedit_kana_, "あ");
}

TEST(TsfTipOnKeyDownPreeditTest, SpaceFlushesPendingRomajiAndIsEatenDuringPreedit) {
  TextServiceHarness h;

  EXPECT_FALSE(h.TestPress(VK_SPACE));
  EXPECT_FALSE(h.Press(VK_SPACE));

  EXPECT_TRUE(h.Press('N'));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_TRUE(h.TestPress(VK_SPACE));
  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_EQ(h.service.preedit_kana_, "ん");

  EXPECT_TRUE(h.TestPress(VK_SPACE));
  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_EQ(h.service.preedit_kana_, "ん");
}

TEST(TsfTipOnKeyDownPreeditTest, SpaceWaitsForLateCandidatesWhenCacheIsEmpty) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_TRUE(h.service.candidate_window_show_pending_for_test());
  EXPECT_TRUE(h.service.shown_candidates_for_test().empty());

  std::vector<azookey::ipc::CandidateField> candidates;
  azookey::ipc::CandidateField candidate;
  candidate.surface = "蚊";
  candidates.push_back(candidate);
  h.service.set_cached_candidates_for_test(std::move(candidates));
  h.service.show_candidate_window_from_cache_for_test();

  EXPECT_FALSE(h.service.candidate_window_show_pending_for_test());
  ASSERT_EQ(h.service.shown_candidates_for_test().size(), 1u);
  EXPECT_EQ(h.service.shown_candidates_for_test()[0].surface, "蚊");
}

TEST(TsfTipOnKeyDownPreeditTest, SpaceUsesCachedCandidatesWithoutPending) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  std::vector<azookey::ipc::CandidateField> candidates;
  azookey::ipc::CandidateField candidate;
  candidate.surface = "蚊";
  candidates.push_back(candidate);
  h.service.set_cached_candidates_for_test(std::move(candidates));

  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_FALSE(h.service.candidate_window_show_pending_for_test());
  ASSERT_EQ(h.service.shown_candidates_for_test().size(), 1u);
  EXPECT_EQ(h.service.shown_candidates_for_test()[0].surface, "蚊");
}

TEST(TsfTipOnKeyDownPreeditTest, NumberRewriterIsOffByDefaultAndAddsAnnotatedCandidatesWhenOn) {
  TextServiceHarness h;
  std::vector<azookey::ipc::CandidateField> host_candidates(2);
  host_candidates[0].surface = "123";
  host_candidates[0].reading = "123";
  host_candidates[1].surface = "0x7b";
  host_candidates[1].reading = "123";

  auto off_items = h.service.candidate_views_for_test("123", host_candidates);
  ASSERT_EQ(off_items.size(), 2u);
  EXPECT_TRUE(off_items[0].description.empty());
  EXPECT_TRUE(off_items[1].description.empty());

  h.service.set_number_rewriter_enabled_for_test(true);
  auto on_items = h.service.candidate_views_for_test("123", host_candidates);
  const auto kanji = std::find_if(on_items.begin(), on_items.end(),
                                  [](const auto& item) { return item.surface == L"百二十三"; });
  ASSERT_NE(kanji, on_items.end());
  EXPECT_EQ(kanji->description, L"漢数字");
  EXPECT_EQ(std::count_if(on_items.begin(), on_items.end(),
                          [](const auto& item) { return item.surface == L"0x7b"; }),
            1);

  const auto all_annotation_items = h.service.candidate_views_for_test("12", {});
  constexpr std::array<std::wstring_view, 8> expected_annotations = {
      L"漢数字", L"大字",  L"丸数字", L"ローマ数字（大文字）", L"ローマ数字（小文字）",
      L"16進数", L"8進数", L"2進数"};
  for (const auto expected : expected_annotations) {
    EXPECT_NE(std::find_if(all_annotation_items.begin(), all_annotation_items.end(),
                           [expected](const auto& item) { return item.description == expected; }),
              all_annotation_items.end())
        << "missing annotation";
  }

  auto mixed_items = h.service.candidate_views_for_test("20世紀", host_candidates);
  EXPECT_EQ(mixed_items.size(), host_candidates.size());
}

TEST(TsfTipOnKeyDownPreeditTest, KatakanaRewriterIsOffByDefaultAndAddsAnnotatedCandidatesWhenOn) {
  TextServiceHarness h;
  std::vector<azookey::ipc::CandidateField> host_candidates(1);
  host_candidates[0].surface = "林檎";
  host_candidates[0].reading = "あっぷる";

  auto off_items = h.service.candidate_views_for_test("あっぷる", host_candidates);
  ASSERT_EQ(off_items.size(), 1u);

  h.service.set_katakana_rewriter_enabled_for_test(true);
  auto on_items = h.service.candidate_views_for_test("あっぷる", host_candidates);
  ASSERT_EQ(on_items.size(), 3u);
  EXPECT_EQ(on_items[0].surface, L"林檎");
  EXPECT_EQ(on_items[1].surface, L"アップル");
  EXPECT_EQ(on_items[1].description, L"全角カタカナ");
  EXPECT_EQ(on_items[2].surface, L"ｱｯﾌﾟﾙ");
  EXPECT_EQ(on_items[2].description, L"半角カタカナ");

  host_candidates.push_back({"アップル", "あっぷる", 0.0, "test"});
  on_items = h.service.candidate_views_for_test("あっぷる", host_candidates);
  EXPECT_EQ(std::count_if(on_items.begin(), on_items.end(),
                          [](const auto& item) { return item.surface == L"アップル"; }),
            1);

  const auto mixed_items = h.service.candidate_views_for_test("漢じ", host_candidates);
  EXPECT_EQ(mixed_items.size(), host_candidates.size());
}

TEST(TsfTipOnKeyDownPreeditTest, NumberRewriterDigitKeysBuildNumericPreedit) {
  TextServiceHarness h;
  AsciiDecimalDigitTranslationGuard digit_translation;
  h.service.set_number_rewriter_enabled_for_test(true);

  for (const WPARAM key : {'1', '2', '3'}) {
    EXPECT_TRUE(h.TestPress(key));
    EXPECT_TRUE(h.Press(key));
  }
  EXPECT_EQ(h.service.preedit_kana_, "123");
  EXPECT_EQ(h.service.pending_ipc_reading_for_test(), "123");

  h.service.set_rewritten_cached_candidates_for_test(h.service.preedit_kana_, {});
  EXPECT_TRUE(h.Press(VK_SPACE));
  const auto shown = h.service.shown_candidates_for_test();
  EXPECT_NE(std::find_if(shown.begin(), shown.end(),
                         [](const auto& candidate) { return candidate.surface == "百二十三"; }),
            shown.end());
}

TEST(TsfTipOnKeyDownPreeditTest, NumberRewriterNumpadDigitsBuildNumericPreedit) {
  TextServiceHarness h;
  h.service.set_number_rewriter_enabled_for_test(true);

  for (const WPARAM key : {VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3}) {
    EXPECT_TRUE(h.TestPress(key));
    EXPECT_TRUE(h.Press(key));
  }
  EXPECT_EQ(h.service.preedit_kana_, "123");
  EXPECT_EQ(h.service.pending_ipc_reading_for_test(), "123");
}

TEST(TsfTipOnKeyDownPreeditTest, NumberRewriterDoesNotEatShiftedNonDigit) {
  TextServiceHarness h;
  AsciiDecimalDigitTranslationGuard digit_translation;
  h.service.set_number_rewriter_enabled_for_test(true);
  h.keyboard_state.SetDown(VK_SHIFT, true);
  h.keyboard_state.SetDown(VK_LSHIFT, true);

  EXPECT_FALSE(h.TestPress('1'));
  EXPECT_FALSE(h.Press('1'));
  EXPECT_TRUE(h.service.preedit_kana_.empty());
}

TEST(TsfTipOnKeyDownPreeditTest, Win32DigitTranslationMatchesCompatibleKeyboardLayout) {
  KeyboardStateGuard keyboard_state;
  keyboard_state.SetDown(VK_SHIFT, false);
  keyboard_state.SetDown(VK_LSHIFT, false);
  keyboard_state.SetDown(VK_RSHIFT, false);
  if (!CurrentKeyboardLayoutProduces('1', false, L'1') ||
      !CurrentKeyboardLayoutProduces('1', true, L'!')) {
    GTEST_SKIP() << "requires a keyboard layout where 1 and Shift+1 produce 1 and !";
  }

  EXPECT_EQ(azookey::tsf::testing::TranslateAsciiDecimalDigitUsingWin32ForTest('1', 0),
            std::optional<char>('1'));

  keyboard_state.SetDown(VK_SHIFT, true);
  keyboard_state.SetDown(VK_LSHIFT, true);
  EXPECT_EQ(azookey::tsf::testing::TranslateAsciiDecimalDigitUsingWin32ForTest('1', 0),
            std::nullopt);
}

TEST(TsfTipOnKeyDownPreeditTest, NumberRewriterCommitsSurfaceWithoutAnnotation) {
  TextServiceHarness h;
  h.service.preedit_kana_ = "123";
  h.service.set_number_rewriter_enabled_for_test(true);

  std::vector<azookey::ipc::CandidateField> host_candidates(1);
  host_candidates[0].surface = "123";
  host_candidates[0].reading = "123";
  h.service.set_rewritten_cached_candidates_for_test("123", std::move(host_candidates));
  ASSERT_TRUE(h.Press(VK_SPACE));

  const auto shown = h.service.shown_candidates_for_test();
  const auto rewritten = std::find_if(shown.begin(), shown.end(), [](const auto& candidate) {
    return candidate.surface == "百二十三";
  });
  ASSERT_NE(rewritten, shown.end());
  h.service.set_selected_candidate_index_for_test(
      static_cast<int>(std::distance(shown.begin(), rewritten)));

  FakeCompositionAttachment attachment(h);
  EXPECT_EQ(h.service.commit_selected_for_test(&h.context), S_OK);
  EXPECT_EQ(attachment.composition_range.last_text, L"百二十三");
  EXPECT_EQ(attachment.composition_range.last_text.find(L"漢数字"), std::wstring::npos);
  EXPECT_FALSE(h.service.has_pending_commit_observation_for_test());
  EXPECT_FALSE(h.service.last_queued_commit_observation_for_test().has_value());
}

TEST(TsfTipOnKeyDownPreeditTest, CommitObservationExcludesNumberRewriteCandidates) {
  TextServiceHarness h;
  h.service.preedit_kana_ = "123";
  h.service.set_number_rewriter_enabled_for_test(true);

  std::vector<azookey::ipc::CandidateField> host_candidates(1);
  host_candidates[0].surface = "123";
  host_candidates[0].reading = "123";
  host_candidates[0].source = "test";
  h.service.set_rewritten_cached_candidates_for_test("123", std::move(host_candidates));
  ASSERT_TRUE(h.Press(VK_SPACE));
  h.service.set_selected_candidate_index_for_test(0);

  FakeCompositionAttachment attachment(h);
  EXPECT_EQ(h.service.commit_selected_for_test(&h.context), S_OK);

  const auto observation = h.service.last_queued_commit_observation_for_test();
  ASSERT_TRUE(observation.has_value());
  EXPECT_EQ(observation->chosen.surface, "123");
  ASSERT_EQ(observation->shown.size(), 1u);
  EXPECT_EQ(observation->shown[0].surface, "123");
}

TEST(TsfTipOnKeyDownPreeditTest, CommitSelectedQueuesCancelBeforeCommitObservation) {
  TextServiceHarness h;
  ASSERT_TRUE(h.Press('K'));
  ASSERT_TRUE(h.Press('A'));
  ASSERT_TRUE(h.service.has_pending_ipc_query_for_test());

  std::vector<azookey::ipc::CandidateField> host_candidates(1);
  host_candidates[0].surface = "蚊";
  host_candidates[0].reading = "か";
  host_candidates[0].source = "test";
  h.service.set_rewritten_cached_candidates_for_test("か", std::move(host_candidates));
  ASSERT_TRUE(h.Press(VK_SPACE));
  h.service.set_selected_candidate_index_for_test(0);

  FakeCompositionAttachment attachment(h);
  ASSERT_EQ(h.service.commit_selected_for_test(&h.context), S_OK);

  const auto types = h.service.queued_ipc_types_for_test();
  ASSERT_EQ(types.size(), 2u);
  EXPECT_EQ(types[0], azookey::ipc::MessageType::Cancel);
  EXPECT_EQ(types[1], azookey::ipc::MessageType::CommitObservation);
}

TEST(TsfTipOnKeyDownPreeditTest, CommitPreeditAsIsQueuesCancelWithoutObservation) {
  TextServiceHarness h;
  ASSERT_TRUE(h.Press('K'));
  ASSERT_TRUE(h.Press('A'));
  ASSERT_TRUE(h.service.has_pending_ipc_query_for_test());

  FakeCompositionAttachment attachment(h);
  ASSERT_TRUE(h.Press(VK_RETURN));

  const auto types = h.service.queued_ipc_types_for_test();
  ASSERT_EQ(types.size(), 1u);
  EXPECT_EQ(types[0], azookey::ipc::MessageType::Cancel);
  EXPECT_FALSE(h.service.last_queued_commit_observation_for_test().has_value());
}

TEST(TsfTipOnKeyDownPreeditTest, CommitObservationExcludesKatakanaRewriteCandidates) {
  TextServiceHarness h;
  h.service.preedit_kana_ = "かな";
  h.service.set_katakana_rewriter_enabled_for_test(true);

  std::vector<azookey::ipc::CandidateField> host_candidates(1);
  host_candidates[0].surface = "仮名";
  host_candidates[0].reading = "かな";
  host_candidates[0].source = "test";
  h.service.set_rewritten_cached_candidates_for_test("かな", std::move(host_candidates));
  ASSERT_TRUE(h.Press(VK_SPACE));
  h.service.set_selected_candidate_index_for_test(0);

  FakeCompositionAttachment attachment(h);
  EXPECT_EQ(h.service.commit_selected_for_test(&h.context), S_OK);

  const auto observation = h.service.last_queued_commit_observation_for_test();
  ASSERT_TRUE(observation.has_value());
  EXPECT_EQ(observation->chosen.surface, "仮名");
  ASSERT_EQ(observation->shown.size(), 1u);
  EXPECT_EQ(observation->shown[0].surface, "仮名");
}

TEST(TsfTipOnKeyDownPreeditTest, ArrowSelectionCommitsFrozenCandidateSnapshot) {
  TextServiceHarness h;
  FakeCompositionAttachment attachment(h);

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  std::vector<azookey::ipc::CandidateField> candidates(2);
  candidates[0].surface = "蚊";
  candidates[1].surface = "科";
  h.service.set_cached_candidates_for_test(std::move(candidates));

  EXPECT_TRUE(h.Press(VK_SPACE));
  ASSERT_EQ(h.service.shown_candidates_for_test().size(), 2u);

  std::vector<azookey::ipc::CandidateField> late_candidates(2);
  late_candidates[0].surface = "価";
  late_candidates[1].surface = "課";
  h.service.set_cached_candidates_for_test(std::move(late_candidates));

  EXPECT_TRUE(h.Press(VK_DOWN));
  const std::wstring displayed_surface = attachment.composition_range.last_text;
  EXPECT_EQ(displayed_surface, L"科");
  const int set_text_count_before = attachment.composition_range.set_text_count;
  EXPECT_TRUE(h.Press(VK_RETURN));

  EXPECT_EQ(attachment.composition_range.set_text_count, set_text_count_before + 1);
  EXPECT_EQ(attachment.composition_range.last_text, displayed_surface);
  EXPECT_EQ(attachment.composition.end_count, 1);
  EXPECT_TRUE(h.service.shown_candidates_for_test().empty());
}

TEST(TsfTipOnKeyDownPreeditTest, ArrowSelectionUpdatesPreeditAndEscapeRestoresReading) {
  TextServiceHarness h;
  FakeCompositionAttachment attachment(h);

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  std::vector<azookey::ipc::CandidateField> candidates(2);
  candidates[0].surface = "蚊";
  candidates[1].surface = "科";
  h.service.set_cached_candidates_for_test(std::move(candidates));

  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_EQ(attachment.composition_range.last_text, L"蚊");
  EXPECT_TRUE(h.Press(VK_DOWN));
  EXPECT_EQ(attachment.composition_range.last_text, L"科");
  EXPECT_EQ(h.service.preedit_kana_, "か");

  EXPECT_TRUE(h.Press(VK_ESCAPE));
  EXPECT_EQ(attachment.composition_range.last_text, L"か");
  EXPECT_EQ(h.service.preedit_kana_, "か");
}

TEST(TsfTipOnKeyDownPreeditTest, TypingAfterCandidateSelectionRestoresReadingBeforeAppending) {
  TextServiceHarness h;
  FakeCompositionAttachment attachment(h);

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  EXPECT_EQ(h.service.preedit_kana_, "か");

  std::vector<azookey::ipc::CandidateField> candidates(2);
  candidates[0].surface = "蚊";
  candidates[1].surface = "科";
  h.service.set_cached_candidates_for_test(std::move(candidates));

  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_TRUE(h.Press(VK_DOWN));
  EXPECT_EQ(attachment.composition_range.last_text, L"科");
  EXPECT_EQ(h.service.preedit_kana_, "か");

  EXPECT_TRUE(h.Press('A'));
  EXPECT_EQ(attachment.composition_range.last_text, L"かあ");
  EXPECT_EQ(h.service.preedit_kana_, "かあ");
}

TEST(TsfTipOnKeyDownPreeditTest, BackspaceAfterCandidateSelectionEditsReading) {
  TextServiceHarness h;
  FakeCompositionAttachment attachment(h);

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  EXPECT_TRUE(h.Press('N'));
  EXPECT_TRUE(h.Press('A'));
  EXPECT_EQ(h.service.preedit_kana_, "かな");

  std::vector<azookey::ipc::CandidateField> candidates(2);
  candidates[0].surface = "仮名";
  candidates[1].surface = "加奈";
  h.service.set_cached_candidates_for_test(std::move(candidates));

  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_TRUE(h.Press(VK_DOWN));
  EXPECT_EQ(attachment.composition_range.last_text, L"加奈");
  EXPECT_EQ(h.service.preedit_kana_, "かな");

  EXPECT_TRUE(h.Press(VK_BACK));
  EXPECT_EQ(attachment.composition_range.last_text, L"か");
  EXPECT_EQ(h.service.preedit_kana_, "か");
}

TEST(TsfTipOnKeyDownPreeditTest, LateCandidatesUpdatePreeditWhenWindowFirstAppears) {
  TextServiceHarness h;
  FakeCompositionAttachment attachment(h);

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(attachment.composition_range.last_text, L"か");

  EXPECT_TRUE(h.Press(VK_SPACE));
  ASSERT_TRUE(h.service.candidate_window_show_pending_for_test());

  std::vector<azookey::ipc::CandidateField> candidates(2);
  candidates[0].surface = "蚊";
  candidates[1].surface = "科";
  h.service.set_cached_candidates_for_test(std::move(candidates));
  h.service.show_candidate_window_from_cache_for_test();

  EXPECT_EQ(attachment.composition_range.last_text, L"蚊");
  EXPECT_EQ(h.service.preedit_kana_, "か");
}

TEST(TsfTipOnKeyDownPreeditTest, NumberSelectionCommitsCorrespondingCandidate) {
  TextServiceHarness h;
  FakeCompositionAttachment attachment(h);

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));

  std::vector<azookey::ipc::CandidateField> candidates(3);
  candidates[0].surface = "蚊";
  candidates[1].surface = "科";
  candidates[2].surface = "課";
  h.service.set_cached_candidates_for_test(std::move(candidates));

  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_TRUE(h.Press(VK_DOWN));
  const std::wstring highlighted_surface = attachment.composition_range.last_text;
  EXPECT_EQ(highlighted_surface, L"科");
  const int set_text_count_before = attachment.composition_range.set_text_count;

  EXPECT_TRUE(h.Press('3'));

  EXPECT_EQ(attachment.composition_range.set_text_count, set_text_count_before + 1);
  EXPECT_EQ(attachment.composition_range.last_text, L"課");
  EXPECT_NE(attachment.composition_range.last_text, highlighted_surface);
  EXPECT_EQ(attachment.composition.end_count, 1);
  EXPECT_TRUE(h.service.shown_candidates_for_test().empty());
}

TEST(TsfTipOnKeyDownPreeditTest, OutOfRangeSelectionCommitsPreeditAsIs) {
  TextServiceHarness h;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  std::vector<azookey::ipc::CandidateField> candidates(1);
  candidates[0].surface = "蚊";
  h.service.set_cached_candidates_for_test(std::move(candidates));
  EXPECT_TRUE(h.Press(VK_SPACE));

  h.service.set_selected_candidate_index_for_test(4);
  h.context.selection_range = &range;
  h.context.run_edit_session = true;

  EXPECT_TRUE(SUCCEEDED(h.service.commit_selected_for_test(&h.context)));

  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x304b'));
  EXPECT_TRUE(h.service.shown_candidates_for_test().empty());
}

TEST(TsfTipOnKeyDownPreeditTest, ReadingChangesClearPendingCandidateWindowShow) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  EXPECT_TRUE(h.Press(VK_SPACE));
  ASSERT_TRUE(h.service.candidate_window_show_pending_for_test());

  EXPECT_TRUE(h.Press('N'));
  EXPECT_FALSE(h.service.candidate_window_show_pending_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, DeactivateReleasesCompositionWhenSyncCleanupIsRejected) {
  TextServiceHarness h;
  FakeComposition composition;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  h.service.composition_ = &composition;
  h.context.request_result = TF_E_LOCKED;
  h.context.request_session_result = TF_E_LOCKED;

  EXPECT_TRUE(SUCCEEDED(h.service.Deactivate()));

  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(composition.end_count, 0);
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_FALSE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, DeactivateEndsCompositionWhenSyncCleanupRuns) {
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;

  EXPECT_TRUE(SUCCEEDED(h.service.Deactivate()));

  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(composition.end_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x304b'));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_FALSE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, FocusLossEndsCompositionAndClearsStaleState) {
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  EXPECT_TRUE(h.Press(VK_SPACE));
  ASSERT_TRUE(h.service.candidate_window_show_pending_for_test());
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;

  EXPECT_EQ(h.service.OnSetFocus(FALSE), S_OK);

  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(composition.end_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x304b'));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_FALSE(h.service.candidate_window_show_pending_for_test());
  EXPECT_FALSE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, FocusLossCommitsLatestPreeditIntoExistingComposition) {
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  range.last_text = std::wstring(1, L'\x304b');

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('I'));
  ASSERT_EQ(h.service.preedit_kana_, "かき");
  ASSERT_EQ(range.last_text, std::wstring(1, L'\x304b'));
  ASSERT_TRUE(h.service.has_active_context_for_test());

  h.context.run_edit_session = true;

  EXPECT_EQ(h.service.OnSetFocus(FALSE), S_OK);

  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(composition.end_count, 1);
  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(range.last_text, std::wstring({L'\x304b', L'\x304d'}));
  EXPECT_EQ(range.collapse_count, 1);
  EXPECT_EQ(range.last_anchor, TF_ANCHOR_END);
  EXPECT_EQ(h.context.set_selection_count, 1);
  EXPECT_EQ(h.context.last_selection_count, 1u);
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_FALSE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, FocusLossPreservesCompositionWhenLatestPreeditSetTextFails) {
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  range.last_text = std::wstring(1, L'\x304b');
  range.set_text_result = E_FAIL;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('I'));
  ASSERT_EQ(h.service.preedit_kana_, "かき");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  h.context.run_edit_session = true;

  EXPECT_EQ(h.service.OnSetFocus(FALSE), S_OK);

  EXPECT_EQ(h.service.composition_, &composition);
  EXPECT_EQ(composition.end_count, 0);
  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x304b'));
  EXPECT_EQ(h.service.preedit_kana_, "かき");
  EXPECT_TRUE(h.service.has_active_context_for_test());

  h.service.composition_->Release();
  h.service.composition_ = nullptr;
}

TEST(TsfTipOnKeyDownPreeditTest, CommitPreeditAsIsUsesSyncEditSessionAndClearsAfterCommit) {
  TextServiceHarness h;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  h.context.selection_range = &range;
  h.context.run_edit_session = true;

  EXPECT_TRUE(h.Press(VK_RETURN));

  EXPECT_EQ(h.context.last_flags, TF_ES_SYNC | TF_ES_READWRITE);
  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x304b'));
  EXPECT_EQ(range.collapse_count, 1);
  EXPECT_EQ(range.last_anchor, TF_ANCHOR_END);
  EXPECT_EQ(h.context.set_selection_count, 1);
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.service.commit_surface_, "");
  EXPECT_FALSE(h.service.committing_);
}

TEST(TsfTipOnKeyDownPreeditTest, QueuedCommitRetriesBeforeNextInputAndClearsStalePreedit) {
  TextServiceHarness h;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  h.context.request_result = TF_E_LOCKED;
  h.context.request_session_result = TF_E_LOCKED;
  EXPECT_TRUE(h.Press(VK_RETURN));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.committing_);
  ASSERT_EQ(h.service.commit_surface_, "か");

  h.context.request_result = S_OK;
  h.context.request_session_result = S_OK;
  h.context.selection_range = &range;
  h.context.run_edit_session = true;

  EXPECT_TRUE(h.TestPress('K'));
  EXPECT_TRUE(h.Press('K'));

  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x304b'));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.service.commit_surface_, "");
  EXPECT_FALSE(h.service.committing_);

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  EXPECT_EQ(h.service.preedit_kana_, "か");
}

TEST(TsfTipOnKeyDownPreeditTest, QueuedCommitRetryConsumesTriggerKeyAfterSuccess) {
  TextServiceHarness h;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  h.context.request_result = TF_E_LOCKED;
  h.context.request_session_result = TF_E_LOCKED;
  EXPECT_TRUE(h.Press(VK_RETURN));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.committing_);
  ASSERT_EQ(h.service.commit_surface_, "か");

  h.context.request_result = S_OK;
  h.context.request_session_result = S_OK;
  h.context.selection_range = &range;
  h.context.run_edit_session = true;

  EXPECT_TRUE(h.TestPress(VK_BACK));
  EXPECT_TRUE(h.Press(VK_BACK));

  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x304b'));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.service.commit_surface_, "");
  EXPECT_FALSE(h.service.committing_);
}

TEST(TsfTipOnKeyDownPreeditTest,
     QueuedCommitRetriesOnOriginalContextWhenNextKeyUsesDifferentContext) {
  TextServiceHarness h;
  NoopContext next_context;
  FakeRange old_range;
  FakeRange next_range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  h.context.request_result = TF_E_LOCKED;
  h.context.request_session_result = TF_E_LOCKED;
  EXPECT_TRUE(h.Press(VK_RETURN));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.committing_);
  ASSERT_EQ(h.service.commit_surface_, "か");

  h.context.request_result = S_OK;
  h.context.request_session_result = S_OK;
  h.context.selection_range = &old_range;
  h.context.run_edit_session = true;

  next_context.selection_range = &next_range;
  next_context.run_edit_session = true;

  BOOL eaten = FALSE;
  EXPECT_EQ(h.service.OnTestKeyDown(&next_context, 'K', 0, &eaten), S_OK);
  EXPECT_TRUE(eaten);
  eaten = FALSE;
  EXPECT_EQ(h.service.OnKeyDown(&next_context, 'K', 0, &eaten), S_OK);
  EXPECT_TRUE(eaten);

  EXPECT_EQ(old_range.set_text_count, 1);
  EXPECT_EQ(old_range.last_text, std::wstring(1, L'\x304b'));
  EXPECT_EQ(next_range.set_text_count, 0);
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.service.commit_surface_, "");
  EXPECT_FALSE(h.service.committing_);
}

TEST(TsfTipOnKeyDownPreeditTest, QueuedCommitConsumesNextInputWhenRetryIsStillRejected) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  h.context.request_result = TF_E_LOCKED;
  h.context.request_session_result = TF_E_LOCKED;
  EXPECT_TRUE(h.Press(VK_RETURN));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.committing_);
  ASSERT_EQ(h.service.commit_surface_, "か");

  EXPECT_TRUE(h.TestPress('A'));
  EXPECT_TRUE(h.Press('A'));

  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_EQ(h.service.commit_surface_, "か");
  EXPECT_TRUE(h.service.committing_);
}

TEST(TsfTipOnKeyDownPreeditTest, QueuedCommitRetryAllocationFailureReturnsOutOfMemory) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  h.context.request_result = TF_E_LOCKED;
  h.context.request_session_result = TF_E_LOCKED;
  EXPECT_TRUE(h.Press(VK_RETURN));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.committing_);
  ASSERT_EQ(h.service.commit_surface_, "か");

  h.context.request_result = S_OK;
  h.context.request_session_result = S_OK;

  BOOL eaten = TRUE;
  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  EXPECT_EQ(h.service.OnKeyDown(&h.context, 'A', 0, &eaten), E_OUTOFMEMORY);
  EXPECT_EQ(eaten, FALSE);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_EQ(h.service.commit_surface_, "か");
  EXPECT_TRUE(h.service.committing_);
}

TEST(TsfTipOnKeyDownPreeditTest, FocusLossCommitsPendingPreeditBeforeCompositionExists) {
  TextServiceHarness h;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_EQ(h.service.composition_, nullptr);
  ASSERT_TRUE(h.service.has_active_context_for_test());

  h.context.selection_range = &range;
  h.context.run_edit_session = true;

  EXPECT_EQ(h.service.OnSetFocus(FALSE), S_OK);

  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x304b'));
  EXPECT_EQ(range.collapse_count, 1);
  EXPECT_EQ(range.last_anchor, TF_ANCHOR_END);
  EXPECT_EQ(h.context.set_selection_count, 1);
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.service.commit_surface_, "");
  EXPECT_FALSE(h.service.committing_);
  EXPECT_FALSE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, FocusLossCommitsBatchRawRomajiPreviewAsKanaReading) {
  TextServiceHarness h;
  FakeRange range;
  h.service.set_batch_romaji_options_for_test(true, true);

  EXPECT_TRUE(h.Press('N'));
  EXPECT_TRUE(h.Press('I'));
  ASSERT_EQ(h.service.preedit_kana_, "ni");
  ASSERT_EQ(h.service.composition_, nullptr);
  ASSERT_TRUE(h.service.has_active_context_for_test());

  h.context.selection_range = &range;
  h.context.run_edit_session = true;

  EXPECT_EQ(h.service.OnSetFocus(FALSE), S_OK);

  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x306b'));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.service.commit_surface_, "");
  EXPECT_FALSE(h.service.committing_);
  EXPECT_FALSE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, FocusLossCommitsQueuedPreeditCommitBeforeCompositionExists) {
  TextServiceHarness h;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_EQ(h.service.composition_, nullptr);

  EXPECT_TRUE(h.Press(VK_RETURN));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.committing_);
  ASSERT_EQ(h.service.commit_surface_, "か");
  ASSERT_TRUE(h.service.has_active_context_for_test());
  EXPECT_EQ(h.context.last_flags, TF_ES_SYNC | TF_ES_READWRITE);

  h.context.selection_range = &range;
  h.context.run_edit_session = true;

  EXPECT_EQ(h.service.OnSetFocus(FALSE), S_OK);

  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x304b'));
  EXPECT_EQ(range.collapse_count, 1);
  EXPECT_EQ(range.last_anchor, TF_ANCHOR_END);
  EXPECT_EQ(h.context.set_selection_count, 1);
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.service.commit_surface_, "");
  EXPECT_FALSE(h.service.committing_);
  EXPECT_FALSE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, FocusLossPreservesQueuedCommitWhenSyncCommitIsRejected) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_EQ(h.service.composition_, nullptr);

  EXPECT_TRUE(h.Press(VK_RETURN));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.committing_);
  ASSERT_EQ(h.service.commit_surface_, "か");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  h.context.request_result = TF_E_LOCKED;
  h.context.request_session_result = TF_E_LOCKED;

  EXPECT_EQ(h.service.OnSetFocus(FALSE), S_OK);

  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_EQ(h.service.commit_surface_, "か");
  EXPECT_TRUE(h.service.committing_);
  EXPECT_TRUE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, CompositionTerminationPreservesQueuedCommitUntilEditSessionRuns) {
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  composition.AddRef();
  h.service.composition_ = &composition;

  EXPECT_TRUE(h.Press(VK_RETURN));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.committing_);
  ASSERT_EQ(h.service.commit_surface_, "か");
  ASSERT_EQ(h.service.composition_, &composition);

  EXPECT_EQ(h.service.OnCompositionTerminated(1, &composition), S_OK);

  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.service.commit_surface_, "か");
  EXPECT_TRUE(h.service.committing_);

  h.context.selection_range = &range;
  h.context.run_edit_session = true;
  EXPECT_TRUE(SUCCEEDED(h.service.RequestPreeditUpdate(&h.context)));

  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x304b'));
  EXPECT_EQ(range.collapse_count, 1);
  EXPECT_EQ(range.last_anchor, TF_ANCHOR_END);
  EXPECT_EQ(h.context.set_selection_count, 1);
  EXPECT_EQ(h.service.commit_surface_, "");
  EXPECT_FALSE(h.service.committing_);
}

TEST(TsfTipOnKeyDownPreeditTest, CommitEditSessionRestoresQueuedCommitWhenGetRangeFails) {
  TextServiceHarness h;
  FakeComposition composition;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  composition.AddRef();
  composition.get_range_result = TF_E_LOCKED;
  h.service.composition_ = &composition;

  EXPECT_TRUE(h.Press(VK_RETURN));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.committing_);
  ASSERT_EQ(h.service.commit_surface_, "か");

  h.context.run_edit_session = true;
  EXPECT_EQ(h.service.RequestPreeditUpdate(&h.context), TF_E_LOCKED);

  EXPECT_EQ(composition.end_count, 0);
  EXPECT_EQ(h.service.composition_, &composition);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_EQ(h.service.commit_surface_, "か");
  EXPECT_TRUE(h.service.committing_);
}

TEST(TsfTipOnKeyDownPreeditTest, CommitEditSessionRestoresQueuedCommitWhenSetTextFails) {
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  range.set_text_result = E_FAIL;

  EXPECT_TRUE(h.Press(VK_RETURN));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.committing_);
  ASSERT_EQ(h.service.commit_surface_, "か");

  h.context.run_edit_session = true;
  EXPECT_EQ(h.service.RequestPreeditUpdate(&h.context), E_FAIL);

  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(composition.end_count, 0);
  EXPECT_EQ(h.service.composition_, &composition);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_EQ(h.service.commit_surface_, "か");
  EXPECT_TRUE(h.service.committing_);
}

TEST(TsfTipOnKeyDownPreeditTest, CommitPreeditAsIsPreservesQueuedCommitWhenInlineGetRangeFails) {
  TextServiceHarness h;
  FakeComposition composition;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  composition.AddRef();
  composition.get_range_result = TF_E_LOCKED;
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;

  EXPECT_TRUE(h.Press(VK_RETURN));

  EXPECT_EQ(composition.end_count, 0);
  EXPECT_EQ(h.service.composition_, &composition);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_EQ(h.service.commit_surface_, "か");
  EXPECT_TRUE(h.service.committing_);
}

TEST(TsfTipOnKeyDownPreeditTest, CommitSelectedPreservesQueuedCommitWhenInlineSetTextFails) {
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  std::vector<azookey::ipc::CandidateField> candidates;
  azookey::ipc::CandidateField candidate;
  candidate.surface = "蚊";
  candidates.push_back(candidate);
  h.service.set_cached_candidates_for_test(std::move(candidates));
  EXPECT_TRUE(h.Press(VK_SPACE));
  ASSERT_EQ(h.service.shown_candidates_for_test().size(), 1u);

  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  range.set_text_result = E_FAIL;
  h.context.run_edit_session = true;

  h.service.commit_selected_for_test(&h.context);

  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(composition.end_count, 0);
  EXPECT_EQ(h.service.composition_, &composition);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_EQ(h.service.commit_surface_, "蚊");
  EXPECT_TRUE(h.service.committing_);
}

TEST(TsfTipOnKeyDownPreeditTest, CandidateObservationPostsAfterQueuedCommitRetrySucceeds) {
  TextServiceHarness h;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  std::vector<azookey::ipc::CandidateField> candidates;
  azookey::ipc::CandidateField candidate;
  candidate.surface = "蚊";
  candidate.reading = "か";
  candidate.source = "test";
  candidates.push_back(candidate);
  h.service.set_cached_candidates_for_test(std::move(candidates));
  EXPECT_TRUE(h.Press(VK_SPACE));
  ASSERT_EQ(h.service.shown_candidates_for_test().size(), 1u);

  h.context.request_result = TF_E_LOCKED;
  h.context.request_session_result = TF_E_LOCKED;
  h.service.commit_selected_for_test(&h.context);

  ASSERT_TRUE(h.service.committing_);
  ASSERT_EQ(h.service.commit_surface_, "蚊");
  EXPECT_TRUE(h.service.has_pending_commit_observation_for_test());
  EXPECT_FALSE(h.service.last_queued_commit_observation_for_test().has_value());

  h.context.request_result = S_OK;
  h.context.request_session_result = S_OK;
  h.context.selection_range = &range;
  h.context.run_edit_session = true;

  EXPECT_TRUE(h.Press('K'));

  EXPECT_FALSE(h.service.committing_);
  EXPECT_FALSE(h.service.has_pending_commit_observation_for_test());
  auto observation = h.service.last_queued_commit_observation_for_test();
  ASSERT_TRUE(observation.has_value());
  EXPECT_EQ(observation->reading, "か");
  EXPECT_EQ(observation->chosen.surface, "蚊");
  ASSERT_EQ(observation->shown.size(), 1u);
  EXPECT_EQ(observation->shown[0].surface, "蚊");
}

TEST(TsfTipOnKeyDownPreeditTest, SelectedCommitObservationFailureDoesNotRetryCommittedText) {
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");

  std::vector<azookey::ipc::CandidateField> candidates;
  azookey::ipc::CandidateField candidate;
  candidate.surface = "蚊";
  candidate.reading = "か";
  candidate.source = "test";
  candidates.push_back(candidate);
  h.service.set_cached_candidates_for_test(std::move(candidates));
  EXPECT_TRUE(h.Press(VK_SPACE));
  ASSERT_EQ(h.service.shown_candidates_for_test().size(), 1u);

  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  h.context.selection_range = &range;
  h.context.run_edit_session = true;

  azookey::tsf::testing::FailNextPendingCommitObservationForTest();
  h.service.commit_selected_for_test(&h.context);

  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x868a'));
  EXPECT_EQ(composition.end_count, 1);
  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.service.commit_surface_, "");
  EXPECT_FALSE(h.service.committing_);
  EXPECT_FALSE(h.service.has_pending_commit_observation_for_test());
  EXPECT_FALSE(h.service.last_queued_commit_observation_for_test().has_value());

  EXPECT_FALSE(h.Press(VK_RETURN));
  EXPECT_EQ(range.set_text_count, 1);
}

TEST(TsfTipOnKeyDownPreeditTest, FocusLossPreservesPendingPreeditWhenSyncCommitIsRejected) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_EQ(h.service.composition_, nullptr);
  ASSERT_TRUE(h.service.has_active_context_for_test());

  h.context.request_result = TF_E_LOCKED;
  h.context.request_session_result = TF_E_LOCKED;

  EXPECT_EQ(h.service.OnSetFocus(FALSE), S_OK);

  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_EQ(h.service.commit_surface_, "");
  EXPECT_FALSE(h.service.committing_);
  EXPECT_TRUE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, FocusLossPreservesCompositionWhenSyncCleanupIsRejected) {
  TextServiceHarness h;
  FakeComposition composition;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  h.service.composition_ = &composition;
  h.context.request_result = TF_E_LOCKED;
  h.context.request_session_result = TF_E_LOCKED;

  EXPECT_EQ(h.service.OnSetFocus(FALSE), S_OK);

  EXPECT_EQ(h.service.composition_, &composition);
  EXPECT_EQ(composition.end_count, 0);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_TRUE(h.service.has_active_context_for_test());

  h.service.composition_->Release();
  h.service.composition_ = nullptr;
}

TEST(TsfTipOnKeyDownPreeditTest, DocumentMgrFocusRefreshAliasDoesNotCleanupActiveContext) {
  FakeDocumentMgr focused_document;
  FakeDocumentMgr alias_document;
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange range;

  focused_document.context_ = &h.context;
  alias_document.identity_unknown_ = static_cast<ITfDocumentMgr*>(&focused_document);
  h.context.document_mgr_ = &focused_document;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;
  const int request_count = h.context.request_count;

  EXPECT_EQ(h.service.OnSetFocus(&alias_document, &focused_document), S_OK);

  EXPECT_EQ(h.context.request_count, request_count);
  EXPECT_EQ(h.service.composition_, &composition);
  EXPECT_EQ(composition.end_count, 0);
  EXPECT_EQ(range.set_text_count, 0);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_TRUE(h.service.has_active_context_for_test());

  h.service.composition_->Release();
  h.service.composition_ = nullptr;
}

TEST(TsfTipOnKeyDownPreeditTest, UninitBackgroundDocumentMgrDoesNotCleanupActiveContext) {
  FakeDocumentMgr focused_document;
  FakeDocumentMgr background_document;
  TextServiceHarness h;
  FakeComposition composition;

  focused_document.context_ = &h.context;
  h.context.document_mgr_ = &focused_document;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  h.service.composition_ = &composition;
  const int request_count = h.context.request_count;

  EXPECT_EQ(h.service.OnUninitDocumentMgr(&background_document), S_OK);

  EXPECT_EQ(h.context.request_count, request_count);
  EXPECT_EQ(h.service.composition_, &composition);
  EXPECT_EQ(composition.end_count, 0);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_TRUE(h.service.has_active_context_for_test());

  h.service.composition_->Release();
  h.service.composition_ = nullptr;
}

TEST(TsfTipOnKeyDownPreeditTest, UninitActiveDocumentMgrCleansUpActiveContext) {
  FakeDocumentMgr active_document;
  FakeComposition composition;
  FakeRange range;
  TextServiceHarness h;

  active_document.context_ = &h.context;
  h.context.document_mgr_ = &active_document;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;

  EXPECT_EQ(h.service.OnUninitDocumentMgr(&active_document), S_OK);

  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(composition.end_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x304b'));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_FALSE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, PoppingActiveContextEndsCompositionAndClearsStaleState) {
  TextServiceHarness h;
  FakeComposition composition;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;

  EXPECT_EQ(h.service.OnPopContext(&h.context), S_OK);

  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(composition.end_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x304b'));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_FALSE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, PoppingContextAliasCleansUpByComIdentity) {
  TextServiceHarness h;
  NoopContext alias_context;
  FakeComposition composition;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  composition.range_ = &range;
  h.service.composition_ = &composition;

  alias_context.identity_unknown_ = static_cast<ITfContext*>(&h.context);
  alias_context.run_edit_session = true;

  EXPECT_EQ(h.service.OnPopContext(&alias_context), S_OK);

  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(composition.end_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x304b'));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_FALSE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest,
     PoppingActiveContextPreservesCompositionWhenSyncCleanupIsRejected) {
  TextServiceHarness h;
  FakeComposition composition;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  h.service.composition_ = &composition;
  h.context.request_result = TF_E_LOCKED;
  h.context.request_session_result = TF_E_LOCKED;

  EXPECT_EQ(h.service.OnPopContext(&h.context), S_OK);

  EXPECT_EQ(h.service.composition_, &composition);
  EXPECT_EQ(composition.end_count, 0);
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_TRUE(h.service.has_active_context_for_test());

  h.service.composition_->Release();
  h.service.composition_ = nullptr;
}

TEST(TsfTipOnKeyDownPreeditTest, NonPreeditControlKeysAreNotEatenWithoutCandidates) {
  TextServiceHarness h;

  EXPECT_FALSE(h.TestPress(VK_BACK));
  EXPECT_FALSE(h.Press(VK_BACK));
  EXPECT_FALSE(h.TestPress(VK_ESCAPE));
  EXPECT_FALSE(h.Press(VK_ESCAPE));
  EXPECT_FALSE(h.TestPress('1'));
  EXPECT_FALSE(h.Press('1'));
}

TEST(TsfTipOnKeyDownPreeditTest, SystemModifiedKeysPassThroughWithoutStartingPreedit) {
  TextServiceHarness h;

  h.keyboard_state.SetDown(VK_CONTROL, true);
  constexpr WPARAM kCtrlShortcutKeys[] = {'A', 'C', 'L', 'S', 'V'};
  for (WPARAM key : kCtrlShortcutKeys) {
    EXPECT_FALSE(h.TestPress(key));
    EXPECT_FALSE(h.Press(key));
  }
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_EQ(h.context.request_count, 0);

  h.keyboard_state.SetDown(VK_CONTROL, false);
  h.keyboard_state.SetDown(VK_MENU, true);
  EXPECT_FALSE(h.TestPress(VK_SPACE));
  EXPECT_FALSE(h.Press(VK_SPACE));
  EXPECT_FALSE(h.TestPress('F'));
  EXPECT_FALSE(h.Press('F'));
  h.keyboard_state.SetDown(VK_MENU, false);

  h.keyboard_state.SetDown(VK_LWIN, true);
  EXPECT_FALSE(h.TestPress(VK_SPACE));
  EXPECT_FALSE(h.Press(VK_SPACE));
  EXPECT_FALSE(h.TestPress('L'));
  EXPECT_FALSE(h.Press('L'));
  EXPECT_EQ(h.service.preedit_kana_, "");
}

TEST(TsfTipOnKeyDownPreeditTest, SystemModifiedKeysPassThroughDuringPreedit) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, "か");
  const int request_count = h.context.request_count;

  h.keyboard_state.SetDown(VK_CONTROL, true);
  EXPECT_FALSE(h.TestPress('C'));
  EXPECT_FALSE(h.Press('C'));
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_EQ(h.context.request_count, request_count);
  h.keyboard_state.SetDown(VK_CONTROL, false);

  h.keyboard_state.SetDown(VK_MENU, true);
  EXPECT_FALSE(h.TestPress(VK_SPACE));
  EXPECT_FALSE(h.Press(VK_SPACE));
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_EQ(h.context.request_count, request_count);
  h.keyboard_state.SetDown(VK_MENU, false);

  h.keyboard_state.SetDown(VK_RWIN, true);
  EXPECT_FALSE(h.TestPress('L'));
  EXPECT_FALSE(h.Press('L'));
  EXPECT_EQ(h.service.preedit_kana_, "か");
  EXPECT_EQ(h.context.request_count, request_count);
  h.keyboard_state.SetDown(VK_RWIN, false);

  EXPECT_TRUE(h.TestPress('A'));
  EXPECT_TRUE(h.Press('A'));
  EXPECT_EQ(h.service.preedit_kana_, "かあ");
}
