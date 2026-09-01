#pragma once

#include <Windows.h>
#include <ctffunc.h>
#include <msctf.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "azookey/core/RomajiKanaConverter.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/tsf/CandidateUiCoordinator.h"

namespace azookey::tsf {

#ifdef AZOOKEY_TSF_TESTING
namespace testing {
using TranslateOemCompositionCharacterFnForTest = std::optional<WCHAR> (*)(WPARAM, LPARAM);
using TranslateAsciiDecimalDigitFnForTest = std::optional<char> (*)(WPARAM, LPARAM);
using GetGuiThreadInfoFnForTest = BOOL(WINAPI*)(DWORD, PGUITHREADINFO);
using ClientToScreenFnForTest = BOOL(WINAPI*)(HWND, LPPOINT);
using GetPhysicalCursorPosFnForTest = BOOL(WINAPI*)(LPPOINT);
using LogicalToPhysicalPointForPerMonitorDpiFnForTest = BOOL(WINAPI*)(HWND, LPPOINT);
using GetMonitorScalePercentFnForTest = UINT (*)(POINT);

struct CaretAnchorForTest {
  POINT point;
  bool valid;
};

void FailNextComBoundaryAllocationForTest();
void ClearComBoundaryAllocationFailureForTest();
bool ConsumeComBoundaryAllocationFailureForTest();
void FailNextPendingCommitObservationForTest();
void ClearPendingCommitObservationFailureForTest();
bool ConsumePendingCommitObservationFailureForTest();
bool IsExpectedIpcResponseForTest(const ipc::Envelope& response, uint64_t expected_request_id,
                                  ipc::MessageType expected_type);
std::optional<WCHAR> TranslateOemCompositionCharacterUsingWin32ForTest(WPARAM virtual_key,
                                                                       LPARAM key_data);
void SetTranslateOemCompositionCharacterForTest(
    TranslateOemCompositionCharacterFnForTest translate_character);
void ClearTranslateOemCompositionCharacterForTest();
std::optional<char> TranslateAsciiDecimalDigitUsingWin32ForTest(WPARAM virtual_key,
                                                                LPARAM key_data);
void SetTranslateAsciiDecimalDigitForTest(TranslateAsciiDecimalDigitFnForTest translate_digit);
void ClearTranslateAsciiDecimalDigitForTest();
void SetCaretWin32ApiForTest(
    GetGuiThreadInfoFnForTest get_gui_thread_info, ClientToScreenFnForTest client_to_screen,
    GetPhysicalCursorPosFnForTest get_physical_cursor_pos,
    LogicalToPhysicalPointForPerMonitorDpiFnForTest logical_to_physical_point,
    GetMonitorScalePercentFnForTest get_monitor_scale_percent);
void ClearCaretWin32ApiForTest();
CaretAnchorForTest ResolveCaretAnchorForTest(const RECT* text_ext_rect,
                                             HWND text_extent_window = nullptr);
}  // namespace testing
#endif

class EditSession;

struct TipCandidate {
  ipc::CandidateField field;
  std::string description;
};

class TextService final : public ITfTextInputProcessorEx,
                          public ITfKeyEventSink,
                          public ITfThreadMgrEventSink,
                          public ITfCompositionSink,
                          public ITfDisplayAttributeProvider,
                          public ITfFnConfigure {
 public:
  TextService();
  ~TextService();

  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override;
  STDMETHODIMP Deactivate() override;
  STDMETHODIMP ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) override;

  STDMETHODIMP OnSetFocus(BOOL foreground) override;
  STDMETHODIMP OnTestKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam,
                             BOOL* eaten) override;
  STDMETHODIMP OnTestKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) override;
  STDMETHODIMP OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) override;
  STDMETHODIMP OnKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) override;
  STDMETHODIMP OnPreservedKey(ITfContext* context, REFGUID rguid, BOOL* eaten) override;

  STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pdim) override;
  STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pdim) override;
  STDMETHODIMP OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus) override;
  STDMETHODIMP OnPushContext(ITfContext* pic) override;
  STDMETHODIMP OnPopContext(ITfContext* pic) override;

  STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

  STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) override;
  STDMETHODIMP GetDisplayAttributeInfo(REFGUID guidInfo, ITfDisplayAttributeInfo** ppInfo) override;

  STDMETHODIMP GetDisplayName(BSTR* name) override;
  STDMETHODIMP Show(HWND parent, LANGID langid, REFGUID profile) override;

  HRESULT RequestPreeditUpdate(ITfContext* context, bool* request_accepted = nullptr);

  // True when the active TSF thread runs in UI-less mode (Windows 11 / Office
  // route candidate UI through the application). Sourced from
  // ITfThreadMgrEx::GetActiveFlags in ActivateEx (spec §2.10) and propagated to
  // CandidateUiCoordinator for pbShow-driven candidate UI routing.
  bool ui_less_mode() const { return ui_less_mode_; }

  // Accessed by EditSession.
  std::string preedit_kana_;
  ITfComposition* composition_{nullptr};
  bool committing_{false};
  std::string commit_surface_;
  POINT caret_pt_{0, 0};
  bool caret_pt_valid_{false};

#ifdef AZOOKEY_TSF_TESTING
  bool candidate_window_show_pending_for_test();
  void set_cached_candidates_for_test(std::vector<ipc::CandidateField> candidates);
  void set_rewritten_cached_candidates_for_test(const std::string& reading,
                                                std::vector<ipc::CandidateField> candidates);
  std::vector<ipc::CandidateField> cached_candidates_for_test();
  std::vector<ipc::CandidateField> shown_candidates_for_test() const;
  std::vector<CandidateViewItem> candidate_views_for_test(
      const std::string& reading, std::vector<ipc::CandidateField> candidates) const;
  void set_number_rewriter_enabled_for_test(bool enabled) {
    number_rewriter_.store(enabled, std::memory_order_relaxed);
  }
  void set_selected_candidate_index_for_test(int index) { selected_candidate_idx_ = index; }
  bool has_pending_commit_observation_for_test() const {
    return pending_commit_observation_.has_value();
  }
  std::optional<ipc::CommitObservationRequest> last_queued_commit_observation_for_test();
  void show_candidate_window_from_cache_for_test();
  bool has_active_context_for_test() const { return active_context_ != nullptr; }
  bool active_context_is_for_test(ITfContext* context) const { return active_context_ == context; }
  HRESULT commit_selected_for_test(ITfContext* context) { return CommitSelected(context); }
  HRESULT request_commit_edit_session_for_test(ITfContext* context) {
    return RequestCommitEditSession(context);
  }
  void set_batch_romaji_options_for_test(bool enabled, bool preview_romaji = false,
                                         bool auto_punctuation = false);
  bool batch_query_in_progress_for_test() const;
  bool has_pending_ipc_query_for_test();
  bool pending_ipc_query_is_batch_for_test();
  uint64_t pending_ipc_request_id_for_test();
  const std::string& ipc_client_id_for_test() const { return ipc_client_id_; }
  std::string pending_ipc_reading_for_test();
  std::string pending_ipc_raw_romaji_for_test();
  void set_ipc_pipe_name_for_test(std::string pipe_name);
  void start_ipc_worker_for_test();
  void stop_ipc_worker_for_test();
  POINT caret_point_for_test() const { return caret_pt_; }
  bool caret_point_valid_for_test() const { return caret_pt_valid_; }
  void set_caret_point_for_test(POINT point, bool valid) {
    caret_pt_ = point;
    caret_pt_valid_ = valid;
  }
#endif

 private:
  friend class EditSession;

  LONG ref_count_{1};
  ITfThreadMgr* thread_mgr_{nullptr};
  TfClientId client_id_{TF_CLIENTID_NULL};
  bool key_event_sink_advised_{false};
  DWORD thread_mgr_sink_cookie_{TF_INVALID_COOKIE};
  bool ui_less_mode_{false};

  core::RomajiKanaConverter romaji_;
  std::string batch_raw_romaji_;
  bool batch_query_in_progress_{false};
  std::atomic<bool> batch_romaji_conversion_{false};
  std::atomic<bool> batch_romaji_preview_romaji_{false};
  std::atomic<bool> batch_conversion_ai_cleanup_{false};
  std::atomic<bool> batch_auto_punctuation_{false};
  std::atomic<bool> number_rewriter_{false};

  // Last context used for preedit updates; allows Deactivate to end composition.
  ITfContext* active_context_{nullptr};
  // Context that owns a queued commit after a sync EditSession rejection.
  ITfContext* commit_context_{nullptr};

  // Candidate UI coordinator (M5).
  CandidateUiCoordinator candidate_ui_;
  int selected_candidate_idx_{0};
  // Snapshot of candidates taken when the window was opened (used for commit
  // so that a late QueryCandidates response cannot change what is confirmed).
  std::vector<TipCandidate> shown_candidates_;
  struct PendingCommitObservation {
    std::string reading;
    ipc::CandidateField chosen;
    std::vector<ipc::CandidateField> shown;
  };
  std::optional<PendingCommitObservation> pending_commit_observation_;

  // IPC worker thread state.
  std::string ipc_client_id_;
  ipc::NamedPipeClient ipc_client_;
  std::mutex ipc_mtx_;
  std::condition_variable ipc_cv_;
  std::thread ipc_thread_;
  std::atomic<bool> ipc_stop_{false};
#ifdef AZOOKEY_TSF_TESTING
  std::string ipc_pipe_name_for_test_;
#endif
  std::string ipc_pending_reading_;
  std::string ipc_pending_raw_romaji_;
  std::string ipc_pending_batch_mode_;
  uint64_t ipc_pending_id_{0};
  bool ipc_has_request_{false};
  bool ipc_pending_is_batch_{false};
  // ID of the QueryCandidates currently sent but not yet received (0 = none).
  // Protected by ipc_mtx_; written by the worker thread, read by TIP thread.
  uint64_t ipc_inflight_id_{0};
  std::string ipc_host_generation_id_;
  bool ipc_has_known_host_generation_{false};

  // Fire-and-forget IPC send queue: CommitObservation, Cancel (M6, M10).
  // For Cancel items, cancel_target_id carries the target_request_id so the
  // worker can detect a cancel that targets the currently in-flight query and
  // abandon its receive (the host returns no response for canceled requests).
  struct IpcSendItem {
    ipc::MessageType type;
    std::string payload_json;
    bool expects_response{false};
    uint64_t cancel_target_id{0};
  };
  std::vector<IpcSendItem> ipc_send_queue_;  // protected by ipc_mtx_

  // Latest Host candidates plus optional TIP-local rewrites (written by IPC
  // thread, read by TIP thread).
  std::mutex candidates_mtx_;
  std::vector<TipCandidate> candidates_;
  bool candidate_window_show_pending_{false};  // protected by candidates_mtx_

  void StartIpcWorker();
  void StopIpcWorker();
  HRESULT AdviseTextServiceSinks();
  HRESULT UnadviseTextServiceSinks();
  std::string IpcPipeName() const;
  void IpcWorkerThread();
  void ServeConnection();
  bool PerformHandshake();
  bool PerformHandshake(ipc::NamedPipeClient& client, uint32_t timeout_ms,
                        const std::string& trace_id, bool update_host_options);
  bool SendCancelOutOfBand(uint64_t target_request_id);
  bool SendCancelOutOfBand(uint64_t target_request_id, uint32_t connect_timeout_ms,
                           uint32_t handshake_timeout_ms);
  bool WaitForReconnectOrStop(uint32_t delay_ms);
  bool WaitForIpcResponseOrStop(uint32_t timeout_ms, uint64_t expected_request_id,
                                ipc::MessageType expected_type);
  bool ObserveHostGeneration(const std::string& host_generation_id);
  void RearmPendingQuery(uint64_t req_id);
  void PostQueryCandidates(const std::string& reading);
  void PostBatchConversion(const std::string& reading, const std::string& raw_romaji);
  static void OnCandidatesReady(void* context);
  void ShowCandidateWindowFromCache();
  POINT CandidateAnchorPoint();
  std::string CurrentPreeditSurface() const;
  std::string CurrentDisplayedPreeditSurface() const;
  bool BatchRomajiEnabled() const;
  std::string BatchPreviewSurface() const;
  std::string BatchReadingForConversion() const;
  void RefreshBatchPreeditSurface();
  void ClearBatchState();
  enum class LifecycleCleanupFailurePolicy {
    PreserveComposition,
    ReleaseComposition,
  };
  void ClearCandidateStateForLifecycle();
  void CancelPendingQueriesForLifecycle();
  void SetCommitContext(ITfContext* context);
  void ClearCommitContext();
  void PostPendingCommitObservation();
  void ClearTextStateForLifecycle();
  bool ActiveContextBelongsToDocumentMgr(ITfDocumentMgr* document_mgr) const;
  HRESULT RequestCommitEditSession(ITfContext* context);
  bool RequestLifecycleCommitOrEndComposition(ITfContext* context);
  void CleanupForLifecycleLoss(ITfContext* context, bool release_active_context,
                               LifecycleCleanupFailurePolicy failure_policy);

  // M6: enqueue a CommitObservation to the IPC worker.
  void PostCommitObservation(const std::string& reading, const ipc::CandidateField& chosen,
                             const std::vector<ipc::CandidateField>& shown);
  // M10: enqueue a Cancel message to the IPC worker.
  void PostCancel(uint64_t target_request_id);
  // Internal: push an item onto ipc_send_queue_ and notify the worker.
  void PostIpcSend(ipc::MessageType type, std::string payload, bool expects_response);

  // M5 commit helpers.
  HRESULT CommitSelected(ITfContext* context);
  HRESULT CommitPreeditAsIs(ITfContext* context);
};

class EditSession final : public ITfEditSession {
 public:
  EditSession(TextService* service, ITfContext* context);

  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;
  STDMETHODIMP DoEditSession(TfEditCookie ec) override;

 private:
  LONG ref_count_{1};
  TextService* service_;
  ITfContext* context_;
};

}  // namespace azookey::tsf
