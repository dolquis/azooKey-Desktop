#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <msctf.h>

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

class NoopContext final : public ITfContext {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
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

  STDMETHODIMP RequestEditSession(TfClientId tid,
                                  ITfEditSession* edit_session,
                                  DWORD flags,
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

  STDMETHODIMP GetSelection(TfEditCookie,
                            ULONG,
                            ULONG,
                            TF_SELECTION*,
                            ULONG* fetched) override {
    if (fetched) *fetched = 0;
    return E_NOTIMPL;
  }

  STDMETHODIMP SetSelection(TfEditCookie, ULONG, const TF_SELECTION*) override {
    return E_NOTIMPL;
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

  STDMETHODIMP TrackProperties(const GUID**,
                               ULONG,
                               const GUID**,
                               ULONG,
                               ITfReadOnlyProperty** property) override {
    if (property) *property = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP EnumProperties(IEnumTfProperties** enum_properties) override {
    if (enum_properties) *enum_properties = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr** document_mgr) override {
    if (document_mgr) *document_mgr = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP CreateRangeBackup(TfEditCookie,
                                 ITfRange*,
                                 ITfRangeBackup** backup) override {
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

 private:
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
    if (range) *range = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP ShiftStart(TfEditCookie, ITfRange*) override { return E_NOTIMPL; }

  STDMETHODIMP ShiftEnd(TfEditCookie, ITfRange*) override { return E_NOTIMPL; }

  STDMETHODIMP EndComposition(TfEditCookie) override {
    ++end_count;
    return end_result;
  }

  int end_count{0};
  HRESULT end_result{S_OK};

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

TEST(TsfTipOnKeyDownPreeditTest, AlphabetInputBuildsKanaPreeditAndEatsKeys) {
  TextServiceHarness h;

  EXPECT_TRUE(h.TestPress('K'));
  EXPECT_TRUE(h.Press('K'));
  EXPECT_EQ(h.service.preedit_kana_, "");

  EXPECT_TRUE(h.TestPress('A'));
  EXPECT_TRUE(h.Press('A'));
  EXPECT_EQ(h.service.preedit_kana_, u8"か");
  EXPECT_GE(h.context.request_count, 2);
  EXPECT_EQ(h.context.last_flags, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE);
}

TEST(TsfTipOnKeyDownPreeditTest, BackspaceRemovesPendingRomajiBeforeKana) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.TestPress(VK_BACK));
  EXPECT_TRUE(h.Press(VK_BACK));
  EXPECT_EQ(h.service.preedit_kana_, "");

  EXPECT_TRUE(h.Press('A'));
  EXPECT_EQ(h.service.preedit_kana_, u8"あ");
}

TEST(TsfTipOnKeyDownPreeditTest, BackspaceDeletesOneUtf8KanaFromPreedit) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  EXPECT_TRUE(h.Press('N'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, u8"かな");

  EXPECT_TRUE(h.Press(VK_BACK));
  EXPECT_EQ(h.service.preedit_kana_, u8"か");

  EXPECT_TRUE(h.Press(VK_BACK));
  EXPECT_EQ(h.service.preedit_kana_, "");

  EXPECT_FALSE(h.TestPress(VK_BACK));
  EXPECT_FALSE(h.Press(VK_BACK));
}

TEST(TsfTipOnKeyDownPreeditTest, EscapeClearsPreeditAndPendingRomaji) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, u8"か");

  EXPECT_TRUE(h.TestPress(VK_ESCAPE));
  EXPECT_TRUE(h.Press(VK_ESCAPE));
  EXPECT_EQ(h.service.preedit_kana_, "");

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press(VK_ESCAPE));
  EXPECT_EQ(h.service.preedit_kana_, "");

  EXPECT_TRUE(h.Press('A'));
  EXPECT_EQ(h.service.preedit_kana_, u8"あ");
}

TEST(TsfTipOnKeyDownPreeditTest, SpaceFlushesPendingRomajiAndIsEatenDuringPreedit) {
  TextServiceHarness h;

  EXPECT_FALSE(h.TestPress(VK_SPACE));
  EXPECT_FALSE(h.Press(VK_SPACE));

  EXPECT_TRUE(h.Press('N'));
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_TRUE(h.TestPress(VK_SPACE));
  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_EQ(h.service.preedit_kana_, u8"ん");

  EXPECT_TRUE(h.TestPress(VK_SPACE));
  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_EQ(h.service.preedit_kana_, u8"ん");
}

TEST(TsfTipOnKeyDownPreeditTest, SpaceWaitsForLateCandidatesWhenCacheIsEmpty) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, u8"か");

  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_TRUE(h.service.candidate_window_show_pending_for_test());
  EXPECT_TRUE(h.service.shown_candidates_for_test().empty());

  std::vector<azookey::ipc::CandidateField> candidates;
  azookey::ipc::CandidateField candidate;
  candidate.surface = u8"蚊";
  candidates.push_back(candidate);
  h.service.set_cached_candidates_for_test(std::move(candidates));
  h.service.show_candidate_window_from_cache_for_test();

  EXPECT_FALSE(h.service.candidate_window_show_pending_for_test());
  ASSERT_EQ(h.service.shown_candidates_for_test().size(), 1u);
  EXPECT_EQ(h.service.shown_candidates_for_test()[0].surface, u8"蚊");
}

TEST(TsfTipOnKeyDownPreeditTest, SpaceUsesCachedCandidatesWithoutPending) {
  TextServiceHarness h;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, u8"か");

  std::vector<azookey::ipc::CandidateField> candidates;
  azookey::ipc::CandidateField candidate;
  candidate.surface = u8"蚊";
  candidates.push_back(candidate);
  h.service.set_cached_candidates_for_test(std::move(candidates));

  EXPECT_TRUE(h.Press(VK_SPACE));
  EXPECT_FALSE(h.service.candidate_window_show_pending_for_test());
  ASSERT_EQ(h.service.shown_candidates_for_test().size(), 1u);
  EXPECT_EQ(h.service.shown_candidates_for_test()[0].surface, u8"蚊");
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
  ASSERT_EQ(h.service.preedit_kana_, u8"か");
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

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, u8"か");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;

  EXPECT_TRUE(SUCCEEDED(h.service.Deactivate()));

  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(composition.end_count, 1);
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_FALSE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, FocusLossEndsCompositionAndClearsStaleState) {
  TextServiceHarness h;
  FakeComposition composition;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, u8"か");
  EXPECT_TRUE(h.Press(VK_SPACE));
  ASSERT_TRUE(h.service.candidate_window_show_pending_for_test());
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;

  EXPECT_EQ(h.service.OnSetFocus(FALSE), S_OK);

  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(composition.end_count, 1);
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_FALSE(h.service.candidate_window_show_pending_for_test());
  EXPECT_FALSE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, FocusLossPreservesCompositionWhenSyncCleanupIsRejected) {
  TextServiceHarness h;
  FakeComposition composition;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, u8"か");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  h.service.composition_ = &composition;
  h.context.request_result = TF_E_LOCKED;
  h.context.request_session_result = TF_E_LOCKED;

  EXPECT_EQ(h.service.OnSetFocus(FALSE), S_OK);

  EXPECT_EQ(h.service.composition_, &composition);
  EXPECT_EQ(composition.end_count, 0);
  EXPECT_EQ(h.service.preedit_kana_, u8"か");
  EXPECT_TRUE(h.service.has_active_context_for_test());

  h.service.composition_->Release();
  h.service.composition_ = nullptr;
}

TEST(TsfTipOnKeyDownPreeditTest, PoppingActiveContextEndsCompositionAndClearsStaleState) {
  TextServiceHarness h;
  FakeComposition composition;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, u8"か");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  h.service.composition_ = &composition;
  h.context.run_edit_session = true;

  EXPECT_EQ(h.service.OnPopContext(&h.context), S_OK);

  EXPECT_EQ(h.service.composition_, nullptr);
  EXPECT_EQ(composition.end_count, 1);
  EXPECT_EQ(h.service.preedit_kana_, "");
  EXPECT_FALSE(h.service.has_active_context_for_test());
}

TEST(TsfTipOnKeyDownPreeditTest, PoppingActiveContextPreservesCompositionWhenSyncCleanupIsRejected) {
  TextServiceHarness h;
  FakeComposition composition;

  EXPECT_TRUE(h.Press('K'));
  EXPECT_TRUE(h.Press('A'));
  ASSERT_EQ(h.service.preedit_kana_, u8"か");
  ASSERT_TRUE(h.service.has_active_context_for_test());

  composition.AddRef();
  h.service.composition_ = &composition;
  h.context.request_result = TF_E_LOCKED;
  h.context.request_session_result = TF_E_LOCKED;

  EXPECT_EQ(h.service.OnPopContext(&h.context), S_OK);

  EXPECT_EQ(h.service.composition_, &composition);
  EXPECT_EQ(composition.end_count, 0);
  EXPECT_EQ(h.service.preedit_kana_, u8"か");
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
  ASSERT_EQ(h.service.preedit_kana_, u8"か");
  const int request_count = h.context.request_count;

  h.keyboard_state.SetDown(VK_CONTROL, true);
  EXPECT_FALSE(h.TestPress('C'));
  EXPECT_FALSE(h.Press('C'));
  EXPECT_EQ(h.service.preedit_kana_, u8"か");
  EXPECT_EQ(h.context.request_count, request_count);
  h.keyboard_state.SetDown(VK_CONTROL, false);

  h.keyboard_state.SetDown(VK_MENU, true);
  EXPECT_FALSE(h.TestPress(VK_SPACE));
  EXPECT_FALSE(h.Press(VK_SPACE));
  EXPECT_EQ(h.service.preedit_kana_, u8"か");
  EXPECT_EQ(h.context.request_count, request_count);
  h.keyboard_state.SetDown(VK_MENU, false);

  h.keyboard_state.SetDown(VK_RWIN, true);
  EXPECT_FALSE(h.TestPress('L'));
  EXPECT_FALSE(h.Press('L'));
  EXPECT_EQ(h.service.preedit_kana_, u8"か");
  EXPECT_EQ(h.context.request_count, request_count);
  h.keyboard_state.SetDown(VK_RWIN, false);

  EXPECT_TRUE(h.TestPress('A'));
  EXPECT_TRUE(h.Press('A'));
  EXPECT_EQ(h.service.preedit_kana_, u8"かあ");
}
