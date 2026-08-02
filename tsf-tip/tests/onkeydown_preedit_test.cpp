#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <gtest/gtest.h>
#include <msctf.h>

#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

class FakeRange final : public ITfRange {
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
    *clone = static_cast<ITfRange*>(this);
    AddRef();
    return S_OK;
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

 private:
  LONG ref_count_{1};
};

class NoopContext final : public ITfContext {
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

  STDMETHODIMP SetSelection(TfEditCookie, ULONG selection_count, const TF_SELECTION*) override {
    ++set_selection_count;
    last_selection_count = selection_count;
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
    if (view) *view = nullptr;
    return E_NOTIMPL;
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
  ITfDocumentMgr* document_mgr_{nullptr};
  IUnknown* identity_unknown_{nullptr};
  int set_selection_count{0};
  ULONG last_selection_count{0};
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

class FakeComposition final : public ITfComposition {
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

}  // namespace

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

TEST(TsfTipOnKeyDownPreeditTest, HostGenerationChangeDropsQueryRearmedAfterDisconnect) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-tip-host-generation-test-" + std::to_string(GetCurrentProcessId());
  std::atomic<bool> first_query_received{false};
  std::atomic<bool> replacement_handshake_received{false};
  std::atomic<bool> replacement_query_received{false};

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
        if (req.type == azookey::ipc::MessageType::QueryCandidates) {
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
          replacement_query_received.store(true);
        }
        return std::nullopt;
      }));

  ASSERT_TRUE(WaitUntil([&] { return replacement_handshake_received.load(); }));
  ASSERT_TRUE(WaitUntil([&] {
    return replacement_query_received.load() || !h.service.has_pending_ipc_query_for_test();
  }));
  EXPECT_FALSE(replacement_query_received.load());
  EXPECT_FALSE(h.service.has_pending_ipc_query_for_test());

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

TEST(TsfTipOnKeyDownPreeditTest, ArrowSelectionCommitsFrozenCandidateSnapshot) {
  TextServiceHarness h;
  FakeRange range;

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

  h.context.selection_range = &range;
  h.context.run_edit_session = true;

  EXPECT_TRUE(h.Press(VK_DOWN));
  EXPECT_TRUE(h.Press(VK_RETURN));

  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x79d1'));
  EXPECT_TRUE(h.service.shown_candidates_for_test().empty());
}

TEST(TsfTipOnKeyDownPreeditTest, NumberSelectionCommitsCorrespondingCandidate) {
  TextServiceHarness h;
  FakeRange range;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));

  std::vector<azookey::ipc::CandidateField> candidates(3);
  candidates[0].surface = "蚊";
  candidates[1].surface = "科";
  candidates[2].surface = "課";
  h.service.set_cached_candidates_for_test(std::move(candidates));

  EXPECT_TRUE(h.Press(VK_SPACE));
  h.context.selection_range = &range;
  h.context.run_edit_session = true;

  EXPECT_TRUE(h.Press('2'));

  EXPECT_EQ(range.set_text_count, 1);
  EXPECT_EQ(range.last_text, std::wstring(1, L'\x79d1'));
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
