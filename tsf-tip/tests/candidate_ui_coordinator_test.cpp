#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <Windows.h>
#include <msctf.h>
#include <OleAuto.h>
// clang-format on

#include <gtest/gtest.h>

#include <atomic>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

#include "../src/CandidateSelection.h"
#include "azookey/tsf/CandidateUiCoordinator.h"
#ifdef _DEBUG
#include "azookey/tsf/DebugThreadAffinity.h"
#endif

namespace {

class MockThreadMgrWithUiElementMgr final : public ITfThreadMgr, public ITfUIElementMgr {
 public:
  ~MockThreadMgrWithUiElementMgr() {
    if (element) element->Release();
  }

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfThreadMgr) {
      *ppv = static_cast<ITfThreadMgr*>(this);
    } else if (riid == IID_ITfUIElementMgr && expose_ui_element_mgr) {
      *ppv = static_cast<ITfUIElementMgr*>(this);
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
  STDMETHODIMP CreateDocumentMgr(ITfDocumentMgr** pp) override {
    if (pp) *pp = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP EnumDocumentMgrs(IEnumTfDocumentMgrs** pp) override {
    if (pp) *pp = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetFocus(ITfDocumentMgr** pp) override {
    if (pp) *pp = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP SetFocus(ITfDocumentMgr*) override { return E_NOTIMPL; }
  STDMETHODIMP AssociateFocus(HWND, ITfDocumentMgr*, ITfDocumentMgr** pp) override {
    if (pp) *pp = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP IsThreadFocus(BOOL* pf) override {
    if (pf) *pf = FALSE;
    return S_OK;
  }
  STDMETHODIMP GetFunctionProvider(REFCLSID, ITfFunctionProvider** pp) override {
    if (pp) *pp = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP EnumFunctionProviders(IEnumTfFunctionProviders** pp) override {
    if (pp) *pp = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetGlobalCompartment(ITfCompartmentMgr** pp) override {
    if (pp) *pp = nullptr;
    return E_NOTIMPL;
  }

  // ITfUIElementMgr
  STDMETHODIMP BeginUIElement(ITfUIElement* pElement, BOOL* pbShow,
                              DWORD* pdwUIElementId) override {
    if (!pElement || !pbShow || !pdwUIElementId) return E_POINTER;
    ++begin_count;
    if (element) element->Release();
    element = pElement;
    element->AddRef();
    *pbShow = begin_pb_show;
    *pdwUIElementId = ui_element_id;
    return begin_hr;
  }
  STDMETHODIMP UpdateUIElement(DWORD dwUIElementId) override {
    if (dwUIElementId != ui_element_id) return E_INVALIDARG;
    ++update_count;
    return update_hr;
  }
  STDMETHODIMP EndUIElement(DWORD dwUIElementId) override {
    if (dwUIElementId != ui_element_id) return E_INVALIDARG;
    ++end_count;
    if (element) {
      element->Release();
      element = nullptr;
    }
    return end_hr;
  }
  STDMETHODIMP GetUIElement(DWORD dwUIELementId, ITfUIElement** ppElement) override {
    if (!ppElement) return E_POINTER;
    *ppElement = nullptr;
    if (dwUIELementId != ui_element_id || !element) return E_INVALIDARG;
    element->AddRef();
    *ppElement = element;
    return S_OK;
  }
  STDMETHODIMP EnumUIElements(IEnumTfUIElements** ppEnum) override {
    if (ppEnum) *ppEnum = nullptr;
    return E_NOTIMPL;
  }

  TfClientId client_id{7};
  BOOL begin_pb_show{FALSE};
  HRESULT begin_hr{S_OK};
  HRESULT update_hr{S_OK};
  HRESULT end_hr{S_OK};
  bool expose_ui_element_mgr{true};
  DWORD ui_element_id{123};
  int begin_count{0};
  int update_count{0};
  int end_count{0};
  ITfUIElement* element{nullptr};

 private:
  LONG ref_count_{1};
};

std::vector<azookey::tsf::CandidateViewItem> SampleItems() {
  return {{L"日本語", L"第一候補"}, {L"にほんご", L""}, {L"日本", L"国名"}};
}

ITfCandidateListUIElement* QueryCandidateList(ITfUIElement* element) {
  ITfCandidateListUIElement* candidates = nullptr;
  if (!element) return nullptr;
  if (FAILED(element->QueryInterface(IID_ITfCandidateListUIElement,
                                     reinterpret_cast<void**>(&candidates)))) {
    return nullptr;
  }
  return candidates;
}

struct BeginObservationCapture {
  int count{0};
  std::optional<azookey::tsf::CandidateUiBeginObservation> observation;
};

void CaptureBeginObservation(const azookey::tsf::CandidateUiBeginObservation& observation,
                             void* context) noexcept {
  auto& capture = *static_cast<BeginObservationCapture*>(context);
  ++capture.count;
  capture.observation = observation;
}

}  // namespace

#ifdef _DEBUG
TEST(TsfTipDebugThreadAffinityTest, DistinguishesTheBoundThread) {
  azookey::tsf::DebugThreadAffinity affinity;
  affinity.BindToCurrentThread();
  EXPECT_TRUE(affinity.IsCurrentThread());
  affinity.AssertCurrentThread();

  std::atomic<bool> other_thread_matches{true};
  std::thread other([&] { other_thread_matches.store(affinity.IsCurrentThread()); });
  other.join();
  EXPECT_FALSE(other_thread_matches.load());
}
#endif

TEST(TsfTipCandidateSelectionTest, WrapSelectionHandlesLargeDeltasWithoutOverflow) {
  using azookey::tsf::internal::WrapCandidateSelectionIndex;

  EXPECT_EQ(WrapCandidateSelectionIndex(1, +5, 3), 0);
  EXPECT_EQ(WrapCandidateSelectionIndex(0, -5, 3), 1);
  EXPECT_EQ(WrapCandidateSelectionIndex(1, std::numeric_limits<int>::min(), 3), 2);
  EXPECT_EQ(WrapCandidateSelectionIndex(1, std::numeric_limits<int>::max(), 3), 2);
  EXPECT_EQ(WrapCandidateSelectionIndex(4, +1, 3), 2);
  EXPECT_EQ(WrapCandidateSelectionIndex(0, +1, 0), 0);
}

TEST(TsfTipCandidateUiCoordinatorTest, PbShowFalseNotifiesAppWithCandidateList) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  BeginObservationCapture capture;
  coordinator.SetBeginObserver(&CaptureBeginObservation, &capture);

  POINT pt{10, 20};
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, pt, SampleItems(), 1), S_OK);

  ASSERT_TRUE(capture.observation.has_value());
  EXPECT_EQ(capture.count, 1);
  EXPECT_EQ(capture.observation->result, S_OK);
  EXPECT_FALSE(capture.observation->ui_less);
  EXPECT_TRUE(capture.observation->ui_element_mgr_available);
  EXPECT_TRUE(capture.observation->pb_show_available);
  EXPECT_FALSE(capture.observation->pb_show);
  EXPECT_FALSE(capture.observation->tip_draws);
  EXPECT_EQ(capture.observation->ui_element_id, thread_mgr.ui_element_id);
  EXPECT_TRUE(coordinator.IsShowing());
  EXPECT_EQ(thread_mgr.begin_count, 1);
  EXPECT_EQ(thread_mgr.update_count, 1);

  ITfCandidateListUIElement* candidates = QueryCandidateList(thread_mgr.element);
  ASSERT_NE(candidates, nullptr);

  UINT count = 0;
  EXPECT_EQ(candidates->GetCount(&count), S_OK);
  EXPECT_EQ(count, 3u);
  UINT selection = 0;
  EXPECT_EQ(candidates->GetSelection(&selection), S_OK);
  EXPECT_EQ(selection, 1u);
  DWORD flags = 0;
  EXPECT_EQ(candidates->GetUpdatedFlags(&flags), S_OK);
  EXPECT_EQ(flags & (TF_CLUIE_COUNT | TF_CLUIE_STRING | TF_CLUIE_SELECTION),
            TF_CLUIE_COUNT | TF_CLUIE_STRING | TF_CLUIE_SELECTION);

  BSTR text = nullptr;
  EXPECT_EQ(candidates->GetString(0, &text), S_OK);
  EXPECT_STREQ(text, L"日本語");
  SysFreeString(text);
  ASSERT_EQ(coordinator.items_for_test().size(), 3u);
  EXPECT_EQ(coordinator.items_for_test()[0].surface, L"日本語");
  EXPECT_EQ(coordinator.items_for_test()[0].description, L"第一候補");
  candidates->Release();

  EXPECT_EQ(coordinator.EndUI(), S_OK);
  EXPECT_EQ(thread_mgr.end_count, 1);
}

TEST(TsfTipCandidateUiCoordinatorTest, BeginUiElementFailureReportsHresult) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_hr = E_FAIL;
  azookey::tsf::CandidateUiCoordinator coordinator;
  BeginObservationCapture capture;
  coordinator.SetBeginObserver(&CaptureBeginObservation, &capture);

  POINT pt{10, 20};
  EXPECT_EQ(coordinator.BeginUI(&thread_mgr, pt, SampleItems(), 0), E_FAIL);

  ASSERT_TRUE(capture.observation.has_value());
  EXPECT_EQ(capture.count, 1);
  EXPECT_EQ(capture.observation->result, E_FAIL);
  EXPECT_FALSE(capture.observation->ui_less);
  EXPECT_TRUE(capture.observation->ui_element_mgr_available);
  EXPECT_FALSE(capture.observation->pb_show_available);
  EXPECT_FALSE(coordinator.IsShowing());
}

TEST(TsfTipCandidateUiCoordinatorTest, InitialAppDrawnUpdateFailureEndsUiElement) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  thread_mgr.update_hr = E_FAIL;
  azookey::tsf::CandidateUiCoordinator coordinator;
  BeginObservationCapture capture;
  coordinator.SetBeginObserver(&CaptureBeginObservation, &capture);

  POINT pt{10, 20};
  EXPECT_EQ(coordinator.BeginUI(&thread_mgr, pt, SampleItems(), 0), E_FAIL);
  ASSERT_TRUE(capture.observation.has_value());
  EXPECT_EQ(capture.count, 1);
  EXPECT_EQ(capture.observation->result, E_FAIL);
  EXPECT_TRUE(capture.observation->ui_element_mgr_available);
  EXPECT_TRUE(capture.observation->pb_show_available);
  EXPECT_FALSE(capture.observation->pb_show);
  EXPECT_EQ(capture.observation->ui_element_id, thread_mgr.ui_element_id);
  EXPECT_FALSE(coordinator.IsShowing());
  EXPECT_EQ(thread_mgr.begin_count, 1);
  EXPECT_EQ(thread_mgr.update_count, 1);
  EXPECT_EQ(thread_mgr.end_count, 1);
  EXPECT_EQ(thread_mgr.element, nullptr);
}

TEST(TsfTipCandidateUiCoordinatorTest, UiLessModeRequiresUiElementManager) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.expose_ui_element_mgr = false;
  azookey::tsf::CandidateUiCoordinator coordinator;
  coordinator.SetUiLessMode(true);
  BeginObservationCapture capture;
  coordinator.SetBeginObserver(&CaptureBeginObservation, &capture);

  POINT pt{10, 20};
  EXPECT_EQ(coordinator.BeginUI(&thread_mgr, pt, SampleItems(), 0), E_NOINTERFACE);
  ASSERT_TRUE(capture.observation.has_value());
  EXPECT_EQ(capture.count, 1);
  EXPECT_EQ(capture.observation->result, E_NOINTERFACE);
  EXPECT_TRUE(capture.observation->ui_less);
  EXPECT_FALSE(capture.observation->ui_element_mgr_available);
  EXPECT_FALSE(capture.observation->pb_show_available);
  EXPECT_FALSE(coordinator.IsShowing());
  EXPECT_EQ(thread_mgr.begin_count, 0);
}

TEST(TsfTipCandidateUiCoordinatorTest, MissingUiElementManagerReportsTipWindowFallback) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.expose_ui_element_mgr = false;
  azookey::tsf::CandidateUiCoordinator coordinator;
  BeginObservationCapture capture;
  coordinator.SetBeginObserver(&CaptureBeginObservation, &capture);

  POINT pt{10, 20};
  EXPECT_EQ(coordinator.BeginUI(&thread_mgr, pt, SampleItems(), 0), S_OK);

  ASSERT_TRUE(capture.observation.has_value());
  EXPECT_EQ(capture.count, 1);
  EXPECT_EQ(capture.observation->result, S_OK);
  EXPECT_FALSE(capture.observation->ui_less);
  EXPECT_FALSE(capture.observation->ui_element_mgr_available);
  EXPECT_FALSE(capture.observation->pb_show_available);
  EXPECT_FALSE(capture.observation->pb_show);
  EXPECT_TRUE(capture.observation->tip_draws);
  EXPECT_EQ(capture.observation->ui_element_id, 0xFFFFFFFF);
  EXPECT_TRUE(coordinator.IsShowing());
  EXPECT_EQ(thread_mgr.begin_count, 0);
}

TEST(TsfTipCandidateUiCoordinatorTest, PbShowTrueUsesTipUiWithoutUpdateNotification) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = TRUE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  BeginObservationCapture capture;
  coordinator.SetBeginObserver(&CaptureBeginObservation, &capture);

  POINT pt{10, 20};
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, pt, SampleItems(), 0), S_OK);
  ASSERT_TRUE(capture.observation.has_value());
  EXPECT_EQ(capture.count, 1);
  EXPECT_TRUE(capture.observation->ui_element_mgr_available);
  EXPECT_TRUE(capture.observation->pb_show_available);
  EXPECT_TRUE(capture.observation->pb_show);
  EXPECT_TRUE(capture.observation->tip_draws);
  EXPECT_EQ(thread_mgr.begin_count, 1);
  EXPECT_EQ(thread_mgr.update_count, 0);

  EXPECT_EQ(coordinator.MoveSelection(+1), S_OK);
  EXPECT_EQ(coordinator.GetSelected(), 1);
  EXPECT_EQ(thread_mgr.update_count, 0);

  EXPECT_EQ(coordinator.EndUI(), S_OK);
  EXPECT_EQ(thread_mgr.end_count, 1);
}

TEST(TsfTipCandidateUiCoordinatorTest, MoveSelectionWrapsLargePositiveAndNegativeDeltas) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  azookey::tsf::CandidateUiCoordinator coordinator;

  POINT pt{10, 20};
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, pt, SampleItems(), 1), S_OK);

  EXPECT_EQ(coordinator.MoveSelection(+5), S_OK);
  EXPECT_EQ(coordinator.GetSelected(), 0);

  EXPECT_EQ(coordinator.MoveSelection(-5), S_OK);
  EXPECT_EQ(coordinator.GetSelected(), 1);

  EXPECT_EQ(coordinator.MoveSelection(std::numeric_limits<int>::min()), S_OK);
  EXPECT_EQ(coordinator.GetSelected(), 2);

  EXPECT_EQ(coordinator.EndUI(), S_OK);
  EXPECT_EQ(thread_mgr.end_count, 1);
}

TEST(TsfTipCandidateUiCoordinatorTest, ElementShowFalseSwitchesSubsequentUpdatesToApp) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = TRUE;
  azookey::tsf::CandidateUiCoordinator coordinator;

  POINT pt{10, 20};
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, pt, SampleItems(), 0), S_OK);
  ASSERT_NE(thread_mgr.element, nullptr);

  EXPECT_EQ(thread_mgr.element->Show(FALSE), S_OK);
  EXPECT_EQ(thread_mgr.update_count, 1);

  ITfCandidateListUIElement* candidates = QueryCandidateList(thread_mgr.element);
  ASSERT_NE(candidates, nullptr);
  DWORD flags = 0;
  EXPECT_EQ(candidates->GetUpdatedFlags(&flags), S_OK);
  EXPECT_EQ(flags & (TF_CLUIE_COUNT | TF_CLUIE_STRING | TF_CLUIE_SELECTION),
            TF_CLUIE_COUNT | TF_CLUIE_STRING | TF_CLUIE_SELECTION);
  candidates->Release();

  EXPECT_EQ(coordinator.MoveSelection(+1), S_OK);
  EXPECT_EQ(thread_mgr.update_count, 2);

  EXPECT_EQ(coordinator.EndUI(), S_OK);
  EXPECT_EQ(thread_mgr.end_count, 1);
}
