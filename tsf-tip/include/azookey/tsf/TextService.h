#pragma once

#include <Windows.h>
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
#include "azookey/tsf/CandidateWindow.h"

namespace azookey::tsf {

#ifdef AZOOKEY_TSF_TESTING
namespace testing {
void FailNextComBoundaryAllocationForTest();
void ClearComBoundaryAllocationFailureForTest();
bool ConsumeComBoundaryAllocationFailureForTest();
void FailNextPendingCommitObservationForTest();
void ClearPendingCommitObservationFailureForTest();
bool ConsumePendingCommitObservationFailureForTest();
}  // namespace testing
#endif

class EditSession;

class TextService final : public ITfTextInputProcessorEx,
                          public ITfKeyEventSink,
                          public ITfThreadMgrEventSink,
                          public ITfCompositionSink,
                          public ITfDisplayAttributeProvider {
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
  STDMETHODIMP OnTestKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) override;
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

  HRESULT RequestPreeditUpdate(ITfContext* context, bool* request_accepted = nullptr);

  // True when the active TSF thread runs in UI-less mode (Windows 11 / Office
  // route candidate UI through the application). Sourced from
  // ITfThreadMgrEx::GetActiveFlags in ActivateEx (spec §2.10). Consuming this
  // to suppress our own candidate window is tracked separately (spec §2.8/M21).
  bool ui_less_mode() const { return ui_less_mode_; }

  // Accessed by EditSession.
  std::string preedit_kana_;
  ITfComposition* composition_{nullptr};
  bool committing_{false};
  std::string commit_surface_;
  POINT caret_pt_{0, 0};

#ifdef AZOOKEY_TSF_TESTING
  bool candidate_window_show_pending_for_test();
  void set_cached_candidates_for_test(std::vector<ipc::CandidateField> candidates);
  const std::vector<ipc::CandidateField>& shown_candidates_for_test() const {
    return shown_candidates_;
  }
  bool has_pending_commit_observation_for_test() const {
    return pending_commit_observation_.has_value();
  }
  std::optional<ipc::CommitObservationRequest> last_queued_commit_observation_for_test();
  void show_candidate_window_from_cache_for_test();
  bool has_active_context_for_test() const { return active_context_ != nullptr; }
  bool active_context_is_for_test(ITfContext* context) const { return active_context_ == context; }
  HRESULT commit_selected_for_test(ITfContext* context) { return CommitSelected(context); }
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

  // Last context used for preedit updates; allows Deactivate to end composition.
  ITfContext* active_context_{nullptr};
  // Context that owns a queued commit after a sync EditSession rejection.
  ITfContext* commit_context_{nullptr};

  // Candidate window (M5).
  CandidateWindow candidate_window_;
  int selected_candidate_idx_{0};
  // Snapshot of candidates taken when the window was opened (used for commit
  // so that a late QueryCandidates response cannot change what is confirmed).
  std::vector<ipc::CandidateField> shown_candidates_;
  struct PendingCommitObservation {
    std::string reading;
    ipc::CandidateField chosen;
    std::vector<ipc::CandidateField> shown;
  };
  std::optional<PendingCommitObservation> pending_commit_observation_;

  // IPC worker thread state.
  ipc::NamedPipeClient ipc_client_;
  std::mutex ipc_mtx_;
  std::condition_variable ipc_cv_;
  std::thread ipc_thread_;
  std::atomic<bool> ipc_stop_{false};
  std::string ipc_pending_reading_;
  uint64_t ipc_pending_id_{0};
  bool ipc_has_request_{false};
  // ID of the QueryCandidates currently sent but not yet received (0 = none).
  // Protected by ipc_mtx_; written by the worker thread, read by TIP thread.
  uint64_t ipc_inflight_id_{0};

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

  // Latest candidates from Host (written by IPC thread, read by TIP thread).
  std::mutex candidates_mtx_;
  std::vector<ipc::CandidateField> candidates_;
  bool candidate_window_show_pending_{false};  // protected by candidates_mtx_

  void StartIpcWorker();
  void StopIpcWorker();
  HRESULT AdviseTextServiceSinks();
  HRESULT UnadviseTextServiceSinks();
  void IpcWorkerThread();
  void ServeConnection();
  bool PerformHandshake();
  bool WaitForReconnectOrStop(uint32_t delay_ms);
  bool WaitForIpcResponseOrStop(uint32_t timeout_ms);
  void RearmPendingQuery(uint64_t req_id);
  void PostQueryCandidates(const std::string& reading);
  static void OnCandidatesReady(void* context);
  void ShowCandidateWindowFromCache();
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
  void CleanupForLifecycleLoss(ITfContext* context,
                               bool release_active_context,
                               LifecycleCleanupFailurePolicy failure_policy);

  // M6: enqueue a CommitObservation to the IPC worker.
  void PostCommitObservation(const std::string& reading,
                             const ipc::CandidateField& chosen,
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
