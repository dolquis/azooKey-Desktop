#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <Windows.h>
#include <msctf.h>
#include <OleAuto.h>
// clang-format on

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <vector>

#include "../src/CandidateSelection.h"
#include "azookey/tsf/CandidateUiCoordinator.h"
#include "azookey/tsf/DpiScaling.h"
#include "azookey/tsf/TipRuntimeLog.h"
#ifdef _DEBUG
#include "azookey/tsf/DebugThreadAffinity.h"
#endif

namespace {

class ScopedCandidateRenderLog {
 public:
  explicit ScopedCandidateRenderLog(bool enabled = true) {
    static std::atomic<unsigned> sequence{0};
    const auto component = "candidate-render-test-" + std::to_string(GetCurrentProcessId()) + "-" +
                           std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++);
    root_ = std::filesystem::temp_directory_path() / component;
    azookey::logging::RuntimeLoggerOptions options;
    options.enabled = enabled;
    options.body_opt_in = true;
    options.component = component;  // Isolates the OS mutex from production TIP and other tests.
    options.logs_directory = root_;
    options.output_path = root_ / "render.jsonl";
    logger_ = std::make_unique<azookey::logging::RuntimeLogger>(options);
    azookey::tsf::SetTipRuntimeLoggerForTest(logger_.get());
  }
  ~ScopedCandidateRenderLog() {
    azookey::tsf::CandidateWindow::SetRenderFailureForTest(
        azookey::tsf::CandidateWindow::RenderFailureForTest::None);
    azookey::tsf::SetTipRuntimeLoggerForTest(nullptr);
    logger_.reset();
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }
  std::string Read() const {
    std::ifstream input(root_ / "render.jsonl", std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

 private:
  std::filesystem::path root_;
  std::unique_ptr<azookey::logging::RuntimeLogger> logger_;
};

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
    if (on_begin) on_begin();
    *pbShow = begin_pb_show;
    *pdwUIElementId = ui_element_id;
    return begin_hr;
  }
  STDMETHODIMP UpdateUIElement(DWORD dwUIElementId) override {
    if (dwUIElementId != ui_element_id) return E_INVALIDARG;
    ++update_count;
    if (throw_on_update) throw std::bad_alloc();
    if (on_update) on_update();
    return update_hr;
  }
  STDMETHODIMP EndUIElement(DWORD dwUIElementId) override {
    if (dwUIElementId != ui_element_id) return E_INVALIDARG;
    ++end_count;
    if (on_end) on_end();
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
  bool throw_on_update{false};
  HRESULT end_hr{S_OK};
  bool expose_ui_element_mgr{true};
  DWORD ui_element_id{123};
  int begin_count{0};
  int update_count{0};
  int end_count{0};
  // An app's sink, run inside BeginUIElement / UpdateUIElement / EndUIElement.
  std::function<void()> on_begin;
  std::function<void()> on_update;
  std::function<void()> on_end;
  ITfUIElement* element{nullptr};
  LONG ref_count() const { return ref_count_; }

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

// Spec candidate-rewriter 10.4: the UI-less / integrated list returns the surface only. Joining
// the annotation would leak it into the search string of an integrated inline search.
TEST(TsfTipCandidateUiCoordinatorTest, UiLessListReturnsSurfaceOnlyForAnnotatedRewriterCandidates) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  const std::vector<azookey::tsf::CandidateViewItem> items{
      {L"通常", L""}, {L"「」", L"かぎ括弧"}, {L"😄", L"笑顔"}, {L"『", L"始め二重かぎ括弧"}};
  const POINT pt{10, 20};
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, pt, items, 2), S_OK);

  ITfCandidateListUIElement* candidates = QueryCandidateList(thread_mgr.element);
  ASSERT_NE(candidates, nullptr);
  UINT count = 0;
  ASSERT_EQ(candidates->GetCount(&count), S_OK);
  ASSERT_EQ(count, items.size());
  for (UINT i = 0; i < count; ++i) {
    BSTR text = nullptr;
    ASSERT_EQ(candidates->GetString(i, &text), S_OK);
    EXPECT_EQ(std::wstring(text, SysStringLen(text)), items[i].surface) << i;
    SysFreeString(text);
  }
  UINT selection = 0;
  ASSERT_EQ(candidates->GetSelection(&selection), S_OK);
  EXPECT_EQ(selection, 2u);
  candidates->Release();

  // The annotation is kept for the TIP-drawn window and is not lost by the UI-less path.
  ASSERT_EQ(coordinator.items_for_test().size(), items.size());
  EXPECT_EQ(coordinator.items_for_test()[2].description, L"笑顔");
  EXPECT_EQ(coordinator.EndUI(), S_OK);
}

TEST(TsfTipCandidateUiCoordinatorTest, NoticeUsesDescriptionWithoutChangingCandidateStrings) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  const POINT pt{10, 20};
  ASSERT_EQ(
      coordinator.BeginUI(&thread_mgr, pt, SampleItems(), 1, L"APIキーを再入力してください。"),
      S_OK);
  ASSERT_NE(thread_mgr.element, nullptr);

  BSTR description = nullptr;
  ASSERT_EQ(thread_mgr.element->GetDescription(&description), S_OK);
  EXPECT_STREQ(description, L"APIキーを再入力してください。");
  SysFreeString(description);

  ITfCandidateListUIElement* candidates = QueryCandidateList(thread_mgr.element);
  ASSERT_NE(candidates, nullptr);
  UINT count = 0;
  UINT selection = 0;
  EXPECT_EQ(candidates->GetCount(&count), S_OK);
  EXPECT_EQ(count, 3u);
  EXPECT_EQ(candidates->GetSelection(&selection), S_OK);
  EXPECT_EQ(selection, 1u);
  BSTR surface = nullptr;
  ASSERT_EQ(candidates->GetString(0, &surface), S_OK);
  EXPECT_STREQ(surface, L"日本語");
  SysFreeString(surface);
  candidates->Release();

  ASSERT_EQ(coordinator.EndUI(), S_OK);
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, pt, SampleItems(), 0), S_OK);
  description = nullptr;
  ASSERT_EQ(thread_mgr.element->GetDescription(&description), S_OK);
  EXPECT_STREQ(description, L"azooKey Candidate List");
  SysFreeString(description);
  EXPECT_EQ(coordinator.EndUI(), S_OK);
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

TEST(TsfTipCandidateUiCoordinatorTest, ExceptionAfterRegistrationEndsUiAndReleasesReferences) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  thread_mgr.throw_on_update = true;
  azookey::tsf::CandidateUiCoordinator coordinator;
  BeginObservationCapture capture;
  coordinator.SetBeginObserver(&CaptureBeginObservation, &capture);

  POINT pt{10, 20};
  EXPECT_EQ(coordinator.BeginUI(&thread_mgr, pt, SampleItems(), 0), E_OUTOFMEMORY);
  EXPECT_FALSE(coordinator.IsShowing());
  EXPECT_EQ(thread_mgr.begin_count, 1);
  EXPECT_EQ(thread_mgr.end_count, 1);
  EXPECT_EQ(thread_mgr.element, nullptr);
  EXPECT_EQ(thread_mgr.ref_count(), 1);
  ASSERT_TRUE(capture.observation.has_value());
  EXPECT_EQ(capture.observation->result, E_OUTOFMEMORY);
  EXPECT_TRUE(capture.observation->ui_element_mgr_available);
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

TEST(TsfTipCandidateUiCoordinatorTest, RenderFailureLogsOncePerStageUntilCreateWithoutBody) {
  using azookey::tsf::CandidateWindow;
  const ScopedCandidateRenderLog log;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.expose_ui_element_mgr = false;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  CandidateWindow::SetRenderFailureForTest(CandidateWindow::RenderFailureForTest::Initialize);
  const std::vector<azookey::tsf::CandidateViewItem> items{
      {L"private-candidate-sentinel", L"private-description-sentinel"}};
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, items, 0, L"private-notice-sentinel"),
            S_OK);
  EXPECT_EQ(coordinator.UpdateUI(items, 0), S_OK);
  EXPECT_EQ(coordinator.MoveSelection(1), S_OK);
  EXPECT_FALSE(coordinator.own_window_for_test().IsVisible());
  const auto record = log.Read();
  EXPECT_EQ(std::count(record.begin(), record.end(), '\n'), 1);
  EXPECT_NE(record.find("\"event\":\"candidate_window_render_failed\""), std::string::npos);
  EXPECT_NE(record.find("\"level\":\"warn\""), std::string::npos);
  EXPECT_NE(record.find("\"stage\":\"injected\""), std::string::npos);
  EXPECT_NE(record.find("\"hr\":" + std::to_string(static_cast<int64_t>(E_FAIL))),
            std::string::npos);
  EXPECT_EQ(record.find("private-"), std::string::npos);
  EXPECT_EQ(record.find("surface"), std::string::npos);
  EXPECT_EQ(record.find("description"), std::string::npos);
  EXPECT_EQ(record.find("notice"), std::string::npos);

  // Hide/end do not reset deduplication. Create starts a new rendering lifetime.
  EXPECT_EQ(coordinator.EndUI(), S_OK);
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, items, 0), S_OK);
  EXPECT_EQ(log.Read(), record);
  coordinator.Destroy();
  ASSERT_TRUE(coordinator.Create());
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, items, 0), S_OK);
  const auto recreated = log.Read();
  EXPECT_EQ(std::count(recreated.begin(), recreated.end(), '\n'), 2);
}

TEST(TsfTipCandidateUiCoordinatorTest, DisabledRenderLoggerDoesNotWriteARecord) {
  const ScopedCandidateRenderLog log(false);
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.expose_ui_element_mgr = false;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  azookey::tsf::CandidateWindow::SetRenderFailureForTest(
      azookey::tsf::CandidateWindow::RenderFailureForTest::Initialize);
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  EXPECT_TRUE(log.Read().empty());
  EXPECT_FALSE(coordinator.own_window_for_test().IsVisible());
}

TEST(TsfTipCandidateUiCoordinatorTest, RecoveredFrameDisplaysPendingToastAndBanner) {
  using azookey::tsf::CandidateHealthState;
  using azookey::tsf::CandidateWindow;
  const ScopedCandidateRenderLog log;
  for (int retry = 0; retry < 3; ++retry) {
    SCOPED_TRACE(retry);
    MockThreadMgrWithUiElementMgr thread_mgr;
    thread_mgr.expose_ui_element_mgr = false;
    azookey::tsf::CandidateUiCoordinator coordinator;
    ASSERT_TRUE(coordinator.Create());
    coordinator.SetSecureState(true, true);
    coordinator.SetHealthState(CandidateHealthState::SafeMode, "generation-1");
    CandidateWindow::SetRenderFailureForTest(CandidateWindow::RenderFailureForTest::Initialize);
    ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
    ASSERT_TRUE(coordinator.secure_toast_pending_for_test());
    ASSERT_TRUE(coordinator.health_banner_pending_for_test());
    const auto& window = coordinator.own_window_for_test();
    ASSERT_FALSE(window.IsVisible());
    CandidateWindow::SetRenderFailureForTest(CandidateWindow::RenderFailureForTest::None);
    if (retry == 0) {
      EXPECT_EQ(coordinator.MoveSelection(1), S_OK);
    } else if (retry == 1) {
      SendMessageW(window.hwnd_for_test(), WM_THEMECHANGED, 0, 0);
    } else {
      coordinator.SetRetryInFlight(true);
    }
    EXPECT_TRUE(window.IsVisible());
    EXPECT_TRUE(window.secure_toast_visible_for_test());
    EXPECT_TRUE(window.health_banner_visible_for_test());
    EXPECT_FALSE(coordinator.secure_toast_pending_for_test());
    EXPECT_FALSE(coordinator.health_banner_pending_for_test());
    // The restored SafeMode banner is still counted once for this Host generation.
    coordinator.SetHealthState(CandidateHealthState::Healthy, "generation-1");
    coordinator.SetHealthState(CandidateHealthState::SafeMode, "generation-1");
    EXPECT_FALSE(coordinator.health_banner_pending_for_test());
    EXPECT_FALSE(window.health_banner_visible_for_test());
    EXPECT_EQ(coordinator.EndUI(), S_OK);
    EXPECT_EQ(coordinator.MoveSelection(1), S_OK);
    EXPECT_FALSE(window.IsVisible());
  }
}

TEST(TsfTipCandidateUiCoordinatorTest, ReplacingHealthBannerKeepsTheNewStateAndResumeDeadline) {
  using azookey::tsf::CandidateHealthState;
  const ScopedCandidateRenderLog log;
  for (const auto initial : {CandidateHealthState::DegradedModel, CandidateHealthState::SafeMode}) {
    SCOPED_TRACE(static_cast<int>(initial));
    MockThreadMgrWithUiElementMgr thread_mgr;
    thread_mgr.expose_ui_element_mgr = false;
    azookey::tsf::CandidateUiCoordinator coordinator;
    ASSERT_TRUE(coordinator.Create());
    ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
    coordinator.SetHealthState(initial, "generation-1");
    const auto& window = coordinator.own_window_for_test();
    ASSERT_TRUE(window.health_banner_visible_for_test());

    coordinator.SetHealthState(CandidateHealthState::SafeMode, "generation-2");
    EXPECT_EQ(window.health_state_for_test(), CandidateHealthState::SafeMode);
    EXPECT_TRUE(window.health_banner_visible_for_test());
    EXPECT_FALSE(coordinator.health_banner_pending_for_test());
    // Re-showing the list hides then resumes the new banner within its deadline.
    ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
    EXPECT_EQ(window.health_state_for_test(), CandidateHealthState::SafeMode);
    EXPECT_TRUE(window.health_banner_visible_for_test());
  }
}

TEST(TsfTipCandidateUiCoordinatorTest, HealthBannerQueuesUntilCandidateWindowIsShown) {
  using azookey::tsf::CandidateHealthState;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.expose_ui_element_mgr = false;
  azookey::tsf::CandidateUiCoordinator coordinator;

  coordinator.SetHealthState(CandidateHealthState::DegradedModel, "generation-1");
  EXPECT_TRUE(coordinator.health_banner_pending_for_test());
  ASSERT_TRUE(coordinator.Create());
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  EXPECT_FALSE(coordinator.health_banner_pending_for_test());

  coordinator.SetHealthState(CandidateHealthState::Healthy, "generation-1");
  EXPECT_FALSE(coordinator.health_banner_pending_for_test());
  coordinator.EndUI();
}

TEST(TsfTipCandidateUiCoordinatorTest, SafeModeNotifiesOncePerHostGeneration) {
  using azookey::tsf::CandidateHealthState;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.expose_ui_element_mgr = false;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);

  coordinator.SetHealthState(CandidateHealthState::SafeMode, "generation-1");
  EXPECT_FALSE(coordinator.health_banner_pending_for_test());
  coordinator.SetHealthState(CandidateHealthState::Healthy, "generation-1");
  coordinator.SetHealthState(CandidateHealthState::SafeMode, "generation-1");
  EXPECT_FALSE(coordinator.health_banner_pending_for_test());

  coordinator.EndUI();
  coordinator.SetHealthState(CandidateHealthState::SafeMode, "generation-2");
  EXPECT_TRUE(coordinator.health_banner_pending_for_test());
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  EXPECT_FALSE(coordinator.health_banner_pending_for_test());
  coordinator.EndUI();
}

TEST(TsfTipCandidateUiCoordinatorTest, HealthBannerSurvivesReshowingTheCandidateList) {
  using azookey::tsf::CandidateHealthState;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.expose_ui_element_mgr = false;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  const auto& window = coordinator.own_window_for_test();

  coordinator.SetHealthState(CandidateHealthState::DegradedModel, "generation-1");
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  ASSERT_TRUE(window.health_banner_visible_for_test());
  // A segment move re-begins the UI without closing it first.
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{30, 20}, SampleItems(), 1), S_OK);
  EXPECT_TRUE(window.health_banner_visible_for_test());

  // Closing the list ends the banner; the next list does not bring it back.
  coordinator.EndUI();
  EXPECT_FALSE(window.health_banner_visible_for_test());
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  EXPECT_FALSE(window.health_banner_visible_for_test());

  // A banner hidden by a return to Healthy is not resumed by a re-show.
  coordinator.SetHealthState(CandidateHealthState::Healthy, "generation-1");
  coordinator.SetHealthState(CandidateHealthState::DegradedModel, "generation-1");
  ASSERT_TRUE(window.health_banner_visible_for_test());
  coordinator.SetHealthState(CandidateHealthState::Healthy, "generation-1");
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{30, 20}, SampleItems(), 1), S_OK);
  EXPECT_FALSE(window.health_banner_visible_for_test());
  coordinator.EndUI();
}

TEST(TsfTipCandidateUiCoordinatorTest, SecureEntryQueuesOneToastUntilCandidateWindowIsShown) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.expose_ui_element_mgr = false;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  const auto& window = coordinator.own_window_for_test();

  coordinator.SetSecureState(true, true);
  EXPECT_TRUE(coordinator.secure_toast_pending_for_test());
  EXPECT_TRUE(window.secure_indicator_visible_for_test());
  EXPECT_FALSE(window.secure_toast_visible_for_test());

  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  EXPECT_FALSE(coordinator.secure_toast_pending_for_test());
  EXPECT_TRUE(window.secure_toast_visible_for_test());

  // Staying secure neither queues nor repeats the toast.
  coordinator.EndUI();
  EXPECT_FALSE(window.secure_toast_visible_for_test());
  coordinator.SetSecureState(true, true);
  EXPECT_FALSE(coordinator.secure_toast_pending_for_test());
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  EXPECT_FALSE(window.secure_toast_visible_for_test());
  EXPECT_TRUE(window.secure_indicator_visible_for_test());
  coordinator.EndUI();
}

TEST(TsfTipCandidateUiCoordinatorTest, LeavingSecureClearsLockAndReentryToastsAgain) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.expose_ui_element_mgr = false;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  const auto& window = coordinator.own_window_for_test();
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);

  coordinator.SetSecureState(true, true);
  EXPECT_TRUE(window.secure_toast_visible_for_test());
  coordinator.SetSecureState(false, true);
  EXPECT_FALSE(window.secure_toast_visible_for_test());
  EXPECT_FALSE(window.secure_indicator_visible_for_test());
  EXPECT_FALSE(coordinator.secure_toast_pending_for_test());

  coordinator.SetSecureState(true, true);
  EXPECT_TRUE(window.secure_toast_visible_for_test());
  EXPECT_TRUE(window.secure_indicator_visible_for_test());
  coordinator.EndUI();

  // A toast still queued when secure ends is dropped, not shown on return.
  coordinator.SetSecureState(false, true);
  coordinator.SetSecureState(true, true);
  coordinator.SetSecureState(false, true);
  EXPECT_FALSE(coordinator.secure_toast_pending_for_test());
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  EXPECT_FALSE(window.secure_toast_visible_for_test());
  coordinator.EndUI();
}

TEST(TsfTipCandidateUiCoordinatorTest, HiddenSecureIndicatorSuppressesToastAndLock) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.expose_ui_element_mgr = false;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  const auto& window = coordinator.own_window_for_test();

  coordinator.SetSecureState(true, false);
  EXPECT_FALSE(coordinator.secure_toast_pending_for_test());
  EXPECT_FALSE(window.secure_indicator_visible_for_test());

  // Turning the setting on mid-secure shows the lock, but it is not an entry.
  coordinator.SetSecureState(true, true);
  EXPECT_FALSE(coordinator.secure_toast_pending_for_test());
  EXPECT_TRUE(window.secure_indicator_visible_for_test());

  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  coordinator.SetSecureState(false, true);
  coordinator.SetSecureState(true, true);
  EXPECT_TRUE(window.secure_toast_visible_for_test());
  coordinator.SetSecureState(true, false);
  EXPECT_FALSE(window.secure_toast_visible_for_test());
  EXPECT_FALSE(window.secure_indicator_visible_for_test());
  coordinator.EndUI();
}

TEST(TsfTipCandidateUiCoordinatorTest, SecureToastWaitsWhileApplicationDrawsCandidates) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  const auto& window = coordinator.own_window_for_test();

  coordinator.SetSecureState(true, true);
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  EXPECT_TRUE(coordinator.secure_toast_pending_for_test());
  EXPECT_FALSE(window.secure_toast_visible_for_test());

  ASSERT_NE(thread_mgr.element, nullptr);
  EXPECT_EQ(thread_mgr.element->Show(TRUE), S_OK);
  EXPECT_FALSE(coordinator.secure_toast_pending_for_test());
  EXPECT_TRUE(window.secure_toast_visible_for_test());
  coordinator.EndUI();
}

TEST(TsfTipCandidateUiCoordinatorTest, SecureToastSurvivesReshowingTheCandidateList) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.expose_ui_element_mgr = false;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  const auto& window = coordinator.own_window_for_test();

  coordinator.SetSecureState(true, true);
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  ASSERT_TRUE(window.secure_toast_visible_for_test());
  // A segment move re-begins the UI without closing it first.
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{30, 20}, SampleItems(), 1), S_OK);
  EXPECT_TRUE(window.secure_toast_visible_for_test());
  coordinator.EndUI();
  EXPECT_FALSE(window.secure_toast_visible_for_test());
}

TEST(TsfTipCandidateUiCoordinatorTest, SecureToastStacksAboveHealthBanner) {
  using azookey::tsf::CandidateHealthState;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.expose_ui_element_mgr = false;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  const auto& window = coordinator.own_window_for_test();

  coordinator.SetHealthState(CandidateHealthState::DegradedModel, "generation-1");
  coordinator.SetSecureState(true, true);
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  EXPECT_TRUE(window.secure_toast_visible_for_test());
  EXPECT_TRUE(window.health_banner_visible_for_test());
  const int row = window.current_metrics_for_test().item_height;
  EXPECT_EQ(window.footer_top_for_test(), row * static_cast<int>(SampleItems().size()));
  EXPECT_EQ(window.health_banner_top_for_test(), window.footer_top_for_test() + row);
  coordinator.EndUI();
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

namespace {

std::vector<azookey::tsf::CandidateViewItem> NumberedItems(int count) {
  std::vector<azookey::tsf::CandidateViewItem> items;
  for (int i = 0; i < count; ++i) items.push_back({L"候補" + std::to_wstring(i), L""});
  return items;
}

class ScopedNativeExposureTestEnvironment {
 public:
  ScopedNativeExposureTestEnvironment() {
    azookey::tsf::CandidateWindow::SetMonitorDpiForTest(96);
    azookey::tsf::CandidateWindow::SetWorkAreaForTest({0, 0, 1920, 1080});
  }
  ~ScopedNativeExposureTestEnvironment() {
    azookey::tsf::CandidateWindow::SetRenderFailureForTest(
        azookey::tsf::CandidateWindow::RenderFailureForTest::None);
    azookey::tsf::CandidateWindow::SetExposureTimeForTest(0);
    azookey::tsf::CandidateWindow::SetWorkAreaForTest({});
    azookey::tsf::CandidateWindow::SetMonitorDpiForTest(0);
  }
};

std::vector<UINT> PageIndex(ITfCandidateListUIElement* candidates) {
  UINT pages = 0;
  if (FAILED(candidates->GetPageIndex(nullptr, 0, &pages))) return {};
  std::vector<UINT> starts(pages);
  UINT filled = 0;
  if (FAILED(candidates->GetPageIndex(starts.data(), pages, &filled)) || filled != pages) return {};
  return starts;
}

UINT CurrentPage(ITfCandidateListUIElement* candidates) {
  UINT page = 0xFFFFFFFF;
  EXPECT_EQ(candidates->GetCurrentPage(&page), S_OK);
  return page;
}

DWORD UpdatedFlags(ITfCandidateListUIElement* candidates) {
  DWORD flags = 0;
  EXPECT_EQ(candidates->GetUpdatedFlags(&flags), S_OK);
  return flags;
}

}  // namespace

TEST(TsfTipCandidateUiCoordinatorTest, NativeExposureUsesFirstDrawForEachVisibleRange) {
  using azookey::tsf::CandidateWindow;
  const ScopedNativeExposureTestEnvironment environment;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = TRUE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  const POINT anchor{10, 20};
  auto items = NumberedItems(12);

  CandidateWindow::SetExposureTimeForTest(100);
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, anchor, items, 0), S_OK);
  auto exposure = coordinator.GetNativeCandidateExposure();
  ASSERT_TRUE(exposure);
  EXPECT_EQ(exposure->shown_at_ms, 100u);
  EXPECT_EQ(exposure->first_row, 0);
  EXPECT_EQ(exposure->row_count, 9);

  CandidateWindow::SetExposureTimeForTest(200);
  ASSERT_EQ(coordinator.SetSelection(1), S_OK);
  exposure = coordinator.GetNativeCandidateExposure();
  ASSERT_TRUE(exposure);
  EXPECT_EQ(exposure->shown_at_ms, 100u);

  // Changing an undrawn row and repainting does not break exposure.
  items[10].surface = L"次ページの候補";
  ASSERT_EQ(coordinator.UpdateUI(items, 1), S_OK);
  exposure = coordinator.GetNativeCandidateExposure();
  ASSERT_TRUE(exposure);
  EXPECT_EQ(exposure->shown_at_ms, 100u);

  ASSERT_EQ(coordinator.SetSelection(9), S_OK);
  exposure = coordinator.GetNativeCandidateExposure();
  ASSERT_TRUE(exposure);
  EXPECT_EQ(exposure->shown_at_ms, 200u);
  EXPECT_EQ(exposure->first_row, 9);
  EXPECT_EQ(exposure->row_count, 3);

  CandidateWindow::SetExposureTimeForTest(300);
  items[10].surface = L"更新した候補";
  ASSERT_EQ(coordinator.UpdateUI(items, 9), S_OK);
  exposure = coordinator.GetNativeCandidateExposure();
  ASSERT_TRUE(exposure);
  EXPECT_EQ(exposure->shown_at_ms, 300u);

  ASSERT_EQ(coordinator.EndUI(), S_OK);
  EXPECT_FALSE(coordinator.GetNativeCandidateExposure());
  CandidateWindow::SetExposureTimeForTest(400);
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, anchor, items, 9), S_OK);
  exposure = coordinator.GetNativeCandidateExposure();
  ASSERT_TRUE(exposure);
  EXPECT_EQ(exposure->shown_at_ms, 400u);
}

TEST(TsfTipCandidateUiCoordinatorTest, NativeExposureTracksResponseIdentityAndRowPositions) {
  using azookey::tsf::CandidateViewItem;
  using azookey::tsf::CandidateWindow;
  const ScopedNativeExposureTestEnvironment environment;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = TRUE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());

  const auto rows_for = [](const char* response_id) {
    auto id = std::make_shared<const std::string>(response_id);
    return std::vector<CandidateViewItem>{{L"候補", L"", id, 0}, {L"候補", L"", id, 1}};
  };
  const auto first_response = rows_for("response-a");
  const auto same_response = rows_for("response-a");
  ASSERT_NE(first_response.front().exposure_id, same_response.front().exposure_id);

  CandidateWindow::SetExposureTimeForTest(100);
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, first_response, 0), S_OK);
  auto exposure = coordinator.GetNativeCandidateExposure();
  ASSERT_TRUE(exposure);
  EXPECT_EQ(exposure->shown_at_ms, 100u);

  CandidateWindow::SetExposureTimeForTest(200);
  ASSERT_EQ(coordinator.UpdateUI(same_response, 0), S_OK);
  exposure = coordinator.GetNativeCandidateExposure();
  ASSERT_TRUE(exposure);
  EXPECT_EQ(exposure->shown_at_ms, 100u);

  auto second_response = rows_for("response-b");
  ASSERT_EQ(coordinator.UpdateUI(second_response, 0), S_OK);
  exposure = coordinator.GetNativeCandidateExposure();
  ASSERT_TRUE(exposure);
  EXPECT_EQ(exposure->shown_at_ms, 200u);

  CandidateWindow::SetExposureTimeForTest(300);
  std::swap(second_response[0].response_index, second_response[1].response_index);
  ASSERT_EQ(coordinator.UpdateUI(second_response, 0), S_OK);
  exposure = coordinator.GetNativeCandidateExposure();
  ASSERT_TRUE(exposure);
  EXPECT_EQ(exposure->shown_at_ms, 300u);
}

TEST(TsfTipCandidateUiCoordinatorTest, NativeExposureRequiresAVisibleNativeCandidateFrame) {
  using azookey::tsf::CandidateWindow;
  const ScopedNativeExposureTestEnvironment environment;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  const POINT anchor{10, 20};
  const auto items = SampleItems();

  CandidateWindow::SetExposureTimeForTest(100);
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, anchor, items, 0), S_OK);
  ASSERT_NE(thread_mgr.element, nullptr);
  EXPECT_FALSE(coordinator.GetNativeCandidateExposure());
  ASSERT_EQ(thread_mgr.element->Show(TRUE), S_OK);
  ASSERT_TRUE(coordinator.GetNativeCandidateExposure());
  ASSERT_EQ(thread_mgr.element->Show(FALSE), S_OK);
  EXPECT_FALSE(coordinator.GetNativeCandidateExposure());

  CandidateWindow::SetExposureTimeForTest(200);
  ASSERT_EQ(thread_mgr.element->Show(TRUE), S_OK);
  CandidateWindow::SetRenderFailureForTest(CandidateWindow::RenderFailureForTest::Draw);
  ASSERT_EQ(coordinator.SetSelection(1), S_OK);
  EXPECT_FALSE(coordinator.GetNativeCandidateExposure());
  CandidateWindow::SetRenderFailureForTest(CandidateWindow::RenderFailureForTest::None);
  CandidateWindow::SetExposureTimeForTest(300);
  ASSERT_EQ(coordinator.SetSelection(0), S_OK);
  const auto exposure = coordinator.GetNativeCandidateExposure();
  ASSERT_TRUE(exposure);
  EXPECT_EQ(exposure->shown_at_ms, 300u);
  coordinator.InvalidateNativeCandidateExposure();
  EXPECT_FALSE(coordinator.GetNativeCandidateExposure());

  ASSERT_EQ(coordinator.EndUI(), S_OK);
  ASSERT_TRUE(coordinator.ShowPredictions(RECT{10, 20, 10, 20}, {L"予測"}));
  ASSERT_TRUE(coordinator.IsShowingPredictions());
  EXPECT_FALSE(coordinator.GetNativeCandidateExposure());
}

// M68 (tsf-deep-integration-spec §2.3): nine candidates to a page.
TEST(TsfTipCandidateUiCoordinatorTest, UiElementPagesAreNineCandidatesAndFollowTheSelection) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, NumberedItems(20), 0), S_OK);
  ITfCandidateListUIElement* candidates = QueryCandidateList(thread_mgr.element);
  ASSERT_NE(candidates, nullptr);

  EXPECT_EQ(PageIndex(candidates), (std::vector<UINT>{0, 9, 18}));
  EXPECT_EQ(UpdatedFlags(candidates) & (TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE),
            static_cast<DWORD>(TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE));
  // A short buffer takes the first pages and still learns the count.
  UINT first_two[2] = {};
  UINT pages = 0;
  EXPECT_EQ(candidates->GetPageIndex(first_two, 2, &pages), S_OK);
  EXPECT_EQ(pages, 3u);
  EXPECT_EQ(first_two[1], 9u);
  EXPECT_EQ(candidates->GetPageIndex(nullptr, 0, nullptr), E_INVALIDARG);
  EXPECT_EQ(CurrentPage(candidates), 0u);

  // Crossing into the second page flags the current page; moving inside it does not.
  EXPECT_EQ(coordinator.SetSelection(10), S_OK);
  EXPECT_EQ(CurrentPage(candidates), 1u);
  EXPECT_EQ(UpdatedFlags(candidates),
            static_cast<DWORD>(TF_CLUIE_SELECTION | TF_CLUIE_CURRENTPAGE));
  EXPECT_EQ(coordinator.SetSelection(11), S_OK);
  EXPECT_EQ(UpdatedFlags(candidates), static_cast<DWORD>(TF_CLUIE_SELECTION));
  EXPECT_EQ(PageIndex(candidates), (std::vector<UINT>{0, 9, 18}));

  // The digits and the page keys go by the same pages.
  EXPECT_TRUE(coordinator.PageSplit().empty());
  EXPECT_EQ(coordinator.DigitTarget(1), 9);
  EXPECT_EQ(coordinator.DigitTarget(9), 17);
  const int update_count = thread_mgr.update_count;
  EXPECT_EQ(coordinator.MovePage(/*forward=*/true), S_OK);
  EXPECT_EQ(coordinator.GetSelected(), 19);  // The short last page's last candidate.
  EXPECT_EQ(CurrentPage(candidates), 2u);
  EXPECT_EQ(thread_mgr.update_count, update_count + 1);
  EXPECT_EQ(coordinator.DigitTarget(3), -1);
  EXPECT_EQ(coordinator.MovePage(/*forward=*/true), S_OK);
  EXPECT_EQ(coordinator.GetSelected(), 1);
  EXPECT_EQ(coordinator.MovePage(/*forward=*/false), S_OK);
  EXPECT_EQ(coordinator.GetSelected(), 19);
  candidates->Release();

  // One page: the page keys change nothing.
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 1), S_OK);
  EXPECT_EQ(coordinator.MovePage(/*forward=*/true), S_FALSE);
  EXPECT_EQ(coordinator.GetSelected(), 1);
  EXPECT_EQ(coordinator.EndUI(), S_OK);
}

TEST(TsfTipCandidateUiCoordinatorTest, SetPageIndexTakesAValidSplitUntilTheListChanges) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, NumberedItems(12), 0), S_OK);
  ITfCandidateListUIElement* candidates = QueryCandidateList(thread_mgr.element);
  ASSERT_NE(candidates, nullptr);

  // Not from 0, not strictly increasing, past the list, or empty.
  for (std::vector<UINT> invalid :
       std::vector<std::vector<UINT>>{{1, 4}, {0, 4, 4}, {0, 6, 4}, {0, 12}}) {
    EXPECT_EQ(candidates->SetPageIndex(invalid.data(), static_cast<UINT>(invalid.size())),
              E_INVALIDARG);
  }
  UINT zero = 0;
  EXPECT_EQ(candidates->SetPageIndex(&zero, 0), E_INVALIDARG);
  EXPECT_EQ(candidates->SetPageIndex(nullptr, 1), E_INVALIDARG);
  EXPECT_EQ(PageIndex(candidates), (std::vector<UINT>{0, 9}));

  std::vector<UINT> split{0, 4, 10};
  EXPECT_EQ(candidates->SetPageIndex(split.data(), static_cast<UINT>(split.size())), S_OK);
  EXPECT_EQ(PageIndex(candidates), split);
  EXPECT_EQ(coordinator.PageSplit(), (std::vector<uint32_t>{0, 4, 10}));
  EXPECT_EQ(coordinator.DigitTarget(4), 3);
  EXPECT_EQ(coordinator.DigitTarget(5), -1);
  EXPECT_EQ(coordinator.MovePage(/*forward=*/true), S_OK);
  EXPECT_EQ(coordinator.GetSelected(), 4);
  EXPECT_EQ(CurrentPage(candidates), 1u);
  EXPECT_EQ(coordinator.DigitTarget(6), 9);
  EXPECT_EQ(coordinator.MovePage(/*forward=*/false), S_OK);
  EXPECT_EQ(coordinator.MovePage(/*forward=*/false), S_OK);
  EXPECT_EQ(coordinator.GetSelected(), 10);

  // A different list drops the split and announces the new pages.
  EXPECT_EQ(coordinator.UpdateUI(NumberedItems(11), 0), S_OK);
  EXPECT_EQ(PageIndex(candidates), (std::vector<UINT>{0, 9}));
  EXPECT_TRUE(coordinator.PageSplit().empty());
  EXPECT_NE(UpdatedFlags(candidates) & TF_CLUIE_PAGEINDEX, 0u);
  candidates->Release();
  EXPECT_EQ(coordinator.EndUI(), S_OK);
}

TEST(TsfTipCandidateUiCoordinatorTest, SplitSetWhileTheTipDrawsAppliesOnceTheAppDraws) {
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = TRUE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, NumberedItems(12), 0), S_OK);
  ITfCandidateListUIElement* candidates = QueryCandidateList(thread_mgr.element);
  ASSERT_NE(candidates, nullptr);

  std::vector<UINT> split{0, 4, 10};
  EXPECT_EQ(candidates->SetPageIndex(split.data(), static_cast<UINT>(split.size())), S_OK);
  // The TIP's window and its digits keep nine to a page.
  EXPECT_TRUE(coordinator.PageSplit().empty());
  EXPECT_EQ(PageIndex(candidates), (std::vector<UINT>{0, 9}));
  EXPECT_EQ(coordinator.DigitTarget(5), 4);

  // Show(FALSE) hands the list to the app with the same items; the split stays.
  EXPECT_EQ(thread_mgr.element->Show(FALSE), S_OK);
  EXPECT_EQ(PageIndex(candidates), split);
  EXPECT_EQ(coordinator.PageSplit(), (std::vector<uint32_t>{0, 4, 10}));
  EXPECT_EQ(coordinator.DigitTarget(5), -1);
  // The split stays with the list: the TIP draws nine to a page again, and the
  // app's split comes back when the app draws the same list again.
  EXPECT_EQ(thread_mgr.element->Show(TRUE), S_OK);
  EXPECT_TRUE(coordinator.PageSplit().empty());
  EXPECT_EQ(PageIndex(candidates), (std::vector<UINT>{0, 9}));
  EXPECT_EQ(thread_mgr.element->Show(FALSE), S_OK);
  EXPECT_EQ(PageIndex(candidates), split);
  EXPECT_EQ(coordinator.PageSplit(), (std::vector<uint32_t>{0, 4, 10}));
  candidates->Release();
  EXPECT_EQ(coordinator.EndUI(), S_OK);
}

// DEV-2104: the window the TIP draws again after the app handed the list back
// opens at the anchor rect BeginUI took, flipped over its top.
TEST(TsfTipCandidateUiCoordinatorTest, RedrawnListKeepsTheAnchorRect) {
  using azookey::tsf::CandidateWindow;
  struct Geometry {
    Geometry() {
      CandidateWindow::SetMonitorDpiForTest(96);
      CandidateWindow::SetWorkAreaForTest({0, 0, 1920, 300});
    }
    ~Geometry() {
      CandidateWindow::SetWorkAreaForTest({});
      CandidateWindow::SetMonitorDpiForTest(0);
    }
  } geometry;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  // A 60 px tall composition 20 px above the bottom of the work area.
  const RECT composition{110, 220, 130, 280};
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, composition, SampleItems(), 0), S_OK);
  EXPECT_FALSE(coordinator.own_window_for_test().IsVisible());

  ASSERT_EQ(thread_mgr.element->Show(TRUE), S_OK);
  const HWND hwnd = coordinator.own_window_for_test().hwnd_for_test();
  ASSERT_TRUE(IsWindowVisible(hwnd)) << coordinator.window_failure_stage();
  RECT bounds{};
  {
    // Physical pixels, as the window was placed; a DPI unaware thread reads them scaled.
    const azookey::tsf::ScopedPerMonitorDpiAwareness physical;
    ASSERT_TRUE(GetWindowRect(hwnd, &bounds));
  }
  EXPECT_EQ(bounds.bottom, composition.top);
  EXPECT_EQ(bounds.left, composition.left);
  EXPECT_EQ(coordinator.EndUI(), S_OK);
  coordinator.Destroy();
}

namespace {

class ScopedRenderFailure {
 public:
  explicit ScopedRenderFailure(azookey::tsf::CandidateWindow::RenderFailureForTest failure) {
    azookey::tsf::CandidateWindow::SetRenderFailureForTest(failure);
  }
  ~ScopedRenderFailure() {
    azookey::tsf::CandidateWindow::SetRenderFailureForTest(
        azookey::tsf::CandidateWindow::RenderFailureForTest::None);
  }
};

const RECT kPredictionAnchor{10, 20, 60, 40};
const std::vector<std::wstring> kPredictions{L"日本", L"日本語"};

}  // namespace

// DEV-1949 (native-ui-spec §4.2): prediction rows draw no footer, so a secure
// toast or health banner raised meanwhile waits for the candidate rows instead
// of running out its five seconds unseen.
TEST(TsfTipCandidateUiCoordinatorTest, PendingToastAndBannerWaitForCandidateRowsAfterPredictions) {
  using azookey::tsf::CandidateHealthState;
  using azookey::tsf::CandidateWindow;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.expose_ui_element_mgr = false;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  const auto& window = coordinator.own_window_for_test();
  ASSERT_TRUE(coordinator.ShowPredictions(kPredictionAnchor, kPredictions));
  ASSERT_TRUE(coordinator.IsShowingPredictions());

  coordinator.SetSecureState(true, true);
  coordinator.SetHealthState(CandidateHealthState::DegradedModel, "generation-1");
  EXPECT_TRUE(coordinator.secure_toast_pending_for_test());
  EXPECT_TRUE(coordinator.health_banner_pending_for_test());
  EXPECT_FALSE(window.secure_toast_visible_for_test());
  EXPECT_FALSE(window.health_banner_visible_for_test());
  // A redrawn prediction frame does not take them either.
  ASSERT_TRUE(coordinator.ShowPredictions(kPredictionAnchor, {L"日本"}));
  EXPECT_TRUE(coordinator.secure_toast_pending_for_test());
  EXPECT_TRUE(coordinator.health_banner_pending_for_test());

  // The candidate rows replace the predictions in a hidden window, then show both.
  const int appearances = window.appearances_for_test();
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 40}, SampleItems(), 0), S_OK);
  EXPECT_EQ(window.mode(), CandidateWindow::Mode::Candidate);
  EXPECT_EQ(window.appearances_for_test(), appearances + 1);
  EXPECT_FALSE(coordinator.IsShowingPredictions());
  EXPECT_FALSE(coordinator.secure_toast_pending_for_test());
  EXPECT_FALSE(coordinator.health_banner_pending_for_test());
  EXPECT_TRUE(window.secure_toast_visible_for_test());
  EXPECT_TRUE(window.health_banner_visible_for_test());
  coordinator.EndUI();
}

// DEV-1949 (native-ui-spec §5.2): predictions never reach the candidate list UI
// element, and the TIP draws them even where the app draws the candidates.
TEST(TsfTipCandidateUiCoordinatorTest, PredictionsStayOutOfTheUiElementAndApartFromTheList) {
  using azookey::tsf::CandidateWindow;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  coordinator.SetUiLessMode(true);
  ASSERT_TRUE(coordinator.Create());
  const auto& window = coordinator.own_window_for_test();

  ASSERT_TRUE(coordinator.ShowPredictions(kPredictionAnchor, kPredictions));
  EXPECT_TRUE(coordinator.IsShowingPredictions());
  EXPECT_EQ(thread_mgr.begin_count, 0);
  EXPECT_EQ(coordinator.GetCount(), 0);
  // Closing the candidate list leaves the predictions alone.
  EXPECT_EQ(coordinator.EndUI(), S_OK);
  EXPECT_TRUE(coordinator.IsShowingPredictions());

  // The app draws the list: the window hides and the element holds the candidates only.
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 40}, SampleItems(), 0), S_OK);
  EXPECT_FALSE(window.IsVisible());
  EXPECT_FALSE(coordinator.IsShowingPredictions());
  ITfCandidateListUIElement* candidates = QueryCandidateList(thread_mgr.element);
  ASSERT_NE(candidates, nullptr);
  UINT count = 0;
  EXPECT_EQ(candidates->GetCount(&count), S_OK);
  EXPECT_EQ(count, SampleItems().size());
  candidates->Release();
  // Predictions wait for the list to close, and hiding them leaves it alone.
  EXPECT_TRUE(coordinator.ShowPredictions(kPredictionAnchor, kPredictions));
  EXPECT_FALSE(window.IsVisible());
  EXPECT_FALSE(coordinator.IsShowingPredictions());
  coordinator.HidePredictions();
  EXPECT_TRUE(coordinator.IsShowing());
  EXPECT_EQ(thread_mgr.end_count, 0);

  ASSERT_EQ(coordinator.EndUI(), S_OK);
  ASSERT_TRUE(coordinator.ShowPredictions(kPredictionAnchor, kPredictions));
  EXPECT_TRUE(coordinator.IsShowingPredictions());
  coordinator.HidePredictions();
  EXPECT_FALSE(window.IsVisible());
}

// DEV-1949 (native-ui-spec §4.2): a prediction frame that failed to draw is not
// shown, and HidePredictions hides only the prediction mode.
TEST(TsfTipCandidateUiCoordinatorTest, PredictionDrawFailureIsNotShowingAndHideSparesCandidates) {
  using azookey::tsf::CandidateWindow;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.expose_ui_element_mgr = false;
  azookey::tsf::CandidateUiCoordinator coordinator;
  ASSERT_TRUE(coordinator.Create());
  const auto& window = coordinator.own_window_for_test();
  {
    const ScopedRenderFailure failure(CandidateWindow::RenderFailureForTest::Draw);
    ASSERT_TRUE(coordinator.ShowPredictions(kPredictionAnchor, kPredictions));
    EXPECT_EQ(window.mode(), CandidateWindow::Mode::Prediction);
    EXPECT_FALSE(coordinator.IsShowingPredictions());
    EXPECT_NE(std::string(coordinator.window_failure_stage()), "");
  }
  coordinator.HidePredictions();

  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 40}, SampleItems(), 0), S_OK);
  ASSERT_TRUE(window.IsVisible());
  coordinator.HidePredictions();
  EXPECT_TRUE(window.IsVisible());
  EXPECT_EQ(window.GetCount(), static_cast<int>(SampleItems().size()));
  coordinator.EndUI();
  EXPECT_FALSE(window.IsVisible());
}

namespace {

ITfCandidateListUIElementBehavior* QueryBehavior(ITfUIElement* element) {
  ITfCandidateListUIElementBehavior* behavior = nullptr;
  if (!element || FAILED(element->QueryInterface(IID_ITfCandidateListUIElementBehavior,
                                                 reinterpret_cast<void**>(&behavior)))) {
    return nullptr;
  }
  return behavior;
}

using BehaviorCalls = std::vector<std::pair<azookey::tsf::CandidateListBehavior, int>>;

// The TIP that holds the coordinator, as far as its reference count goes.
class CountingOwner final : public IUnknown {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid != IID_IUnknown) return E_NOINTERFACE;
    *ppv = this;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
  STDMETHODIMP_(ULONG) Release() override { return --refs; }
  ULONG refs{1};
};

}  // namespace

// tsf-deep-integration-spec §2.3: an app's SetSelection, Finalize and Abort reach
// the TIP, and the element reports the selection the TIP sets, not the app's.
TEST(TsfTipCandidateUiCoordinatorTest, AppBehaviorCallsReachTheTipWhichMovesTheSelection) {
  using azookey::tsf::CandidateListBehavior;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  BehaviorCalls calls;
  HRESULT handler_result = S_OK;
  CountingOwner owner;
  std::vector<ULONG> owner_refs_inside;
  coordinator.SetOnBehavior(
      [&](CandidateListBehavior behavior, int index) {
        calls.emplace_back(behavior, index);
        owner_refs_inside.push_back(owner.refs);
        return handler_result;
      },
      &owner);
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  ITfCandidateListUIElementBehavior* behavior = QueryBehavior(thread_mgr.element);
  ASSERT_NE(behavior, nullptr);

  EXPECT_EQ(behavior->SetSelection(2), S_OK);
  UINT selection = 99;
  EXPECT_EQ(behavior->GetSelection(&selection), S_OK);
  EXPECT_EQ(selection, 0u);
  const int updates = thread_mgr.update_count;
  EXPECT_EQ(coordinator.SetSelection(2), S_OK);
  EXPECT_EQ(thread_mgr.update_count, updates + 1);
  EXPECT_EQ(behavior->GetSelection(&selection), S_OK);
  EXPECT_EQ(selection, 2u);
  EXPECT_EQ(behavior->Finalize(), S_OK);
  EXPECT_EQ(behavior->Abort(), S_OK);
  // An index past the list never reaches the TIP; the TIP's failure reaches the app.
  EXPECT_EQ(behavior->SetSelection(3), E_INVALIDARG);
  handler_result = E_OUTOFMEMORY;
  EXPECT_EQ(behavior->Finalize(), E_OUTOFMEMORY);
  const BehaviorCalls expected{{CandidateListBehavior::SetSelection, 2},
                               {CandidateListBehavior::Finalize, 0},
                               {CandidateListBehavior::Abort, 0},
                               {CandidateListBehavior::Finalize, 0}};
  EXPECT_EQ(calls, expected);
  // Each call holds the owner until it has ended.
  EXPECT_EQ(owner_refs_inside, (std::vector<ULONG>{2, 2, 2, 2}));
  EXPECT_EQ(owner.refs, 1u);
  behavior->Release();
  EXPECT_EQ(coordinator.EndUI(), S_OK);
}

// An app may keep the element after the list ends, also when the TIP ends it
// inside Finalize; the element then reaches no list, nor a later one.
TEST(TsfTipCandidateUiCoordinatorTest, AppBehaviorCallsAfterTheListEndsReachNothing) {
  using azookey::tsf::CandidateListBehavior;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  BehaviorCalls calls;
  coordinator.SetOnBehavior(
      [&](CandidateListBehavior behavior, int index) {
        calls.emplace_back(behavior, index);
        return coordinator.EndUI();
      },
      nullptr);
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 1), S_OK);
  ITfCandidateListUIElementBehavior* behavior = QueryBehavior(thread_mgr.element);
  ASSERT_NE(behavior, nullptr);

  EXPECT_EQ(behavior->Finalize(), S_OK);
  EXPECT_FALSE(coordinator.IsShowing());
  EXPECT_EQ(thread_mgr.end_count, 1);
  EXPECT_EQ(calls.size(), 1u);
  EXPECT_EQ(behavior->Finalize(), E_FAIL);
  EXPECT_EQ(behavior->SetSelection(0), E_FAIL);
  EXPECT_EQ(behavior->Abort(), E_FAIL);

  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  EXPECT_EQ(behavior->Finalize(), E_FAIL);
  EXPECT_TRUE(coordinator.IsShowing());
  EXPECT_EQ(calls.size(), 1u);
  behavior->Release();
  EXPECT_EQ(coordinator.EndUI(), S_OK);
}

// An app's sink may call back from inside Begin/Update/EndUIElement, and the
// TIP's handling of a call notifies the app again; those nested calls get
// E_FAIL and never reach the TIP.
TEST(TsfTipCandidateUiCoordinatorTest, AppBehaviorCallsInsideTheTipsCallsAreRefused) {
  using azookey::tsf::CandidateListBehavior;
  MockThreadMgrWithUiElementMgr thread_mgr;
  thread_mgr.begin_pb_show = FALSE;
  azookey::tsf::CandidateUiCoordinator coordinator;
  BehaviorCalls calls;
  std::vector<HRESULT> nested;
  coordinator.SetOnBehavior(
      [&](CandidateListBehavior behavior, int index) {
        calls.emplace_back(behavior, index);
        ITfCandidateListUIElementBehavior* self = QueryBehavior(thread_mgr.element);
        nested.push_back(self ? self->Abort() : S_OK);
        if (self) self->Release();
        return coordinator.SetSelection(index);
      },
      nullptr);
  const auto call_from_sink = [&] {
    ITfCandidateListUIElementBehavior* self = QueryBehavior(thread_mgr.element);
    nested.push_back(self ? self->Finalize() : S_OK);
    if (self) self->Release();
  };
  // BeginUIElement, and the first UpdateUIElement after it, inside BeginUI.
  thread_mgr.on_begin = call_from_sink;
  thread_mgr.on_update = call_from_sink;
  ASSERT_EQ(coordinator.BeginUI(&thread_mgr, POINT{10, 20}, SampleItems(), 0), S_OK);
  thread_mgr.on_begin = nullptr;
  EXPECT_TRUE(coordinator.IsShowing());
  ITfCandidateListUIElementBehavior* behavior = QueryBehavior(thread_mgr.element);
  ASSERT_NE(behavior, nullptr);

  EXPECT_EQ(behavior->SetSelection(2), S_OK);
  UINT selection = 0;
  EXPECT_EQ(behavior->GetSelection(&selection), S_OK);
  EXPECT_EQ(selection, 2u);
  thread_mgr.on_update = nullptr;
  thread_mgr.on_end = call_from_sink;
  EXPECT_EQ(coordinator.EndUI(), S_OK);
  thread_mgr.on_end = nullptr;
  // Inside BeginUIElement and BeginUI's update, the handler's Abort, the
  // handler's own update, and EndUIElement.
  EXPECT_EQ(nested, (std::vector<HRESULT>{E_FAIL, E_FAIL, E_FAIL, E_FAIL, E_FAIL}));
  EXPECT_EQ(calls, (BehaviorCalls{{CandidateListBehavior::SetSelection, 2}}));
  behavior->Release();
}
