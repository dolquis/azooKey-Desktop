#include "azookey/tsf/TextService.h"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <thread>
#include <utility>

#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/tsf/DisplayAttribute.h"

namespace {

constexpr const char* kTipVersion = "0.1.0";

void DebugLog(const std::string& message) {
#ifdef _DEBUG
  OutputDebugStringA(("[azooKey TIP] " + message + "\n").c_str());
#else
  UNREFERENCED_PARAMETER(message);
#endif
}

std::string IpcHandshakeTokenFromEnv() {
#if defined(_MSC_VER)
  char* value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, "AZOOKEY_IPC_HANDSHAKE_TOKEN") != 0 || value == nullptr) {
    return {};
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  const char* value = std::getenv("AZOOKEY_IPC_HANDSHAKE_TOKEN");
  return value ? std::string(value) : std::string();
#endif
}

std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
  if (len <= 0) return {};
  std::wstring result(len, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), result.data(), len);
  return result;
}

bool IsVirtualKeyDown(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

bool HasSystemModifierDown() {
  return IsVirtualKeyDown(VK_CONTROL) || IsVirtualKeyDown(VK_LCONTROL) ||
         IsVirtualKeyDown(VK_RCONTROL) || IsVirtualKeyDown(VK_MENU) || IsVirtualKeyDown(VK_LMENU) ||
         IsVirtualKeyDown(VK_RMENU) || IsVirtualKeyDown(VK_LWIN) || IsVirtualKeyDown(VK_RWIN);
}

bool SameComIdentity(IUnknown* lhs, IUnknown* rhs) {
  if (lhs == rhs) return true;
  if (!lhs || !rhs) return lhs == rhs;
  IUnknown* lhs_unknown = nullptr;
  IUnknown* rhs_unknown = nullptr;
  const HRESULT lhs_hr = lhs->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&lhs_unknown));
  const HRESULT rhs_hr = rhs->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&rhs_unknown));
  const bool same = SUCCEEDED(lhs_hr) && SUCCEEDED(rhs_hr) && lhs_unknown == rhs_unknown;
  if (lhs_unknown) lhs_unknown->Release();
  if (rhs_unknown) rhs_unknown->Release();
  return same;
}

}  // namespace

namespace azookey::tsf {

TextService::TextService() = default;

TextService::~TextService() {
  StopIpcWorker();
}

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppvObj) {
  if (!ppvObj) return E_INVALIDARG;
  *ppvObj = nullptr;
  if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessor || riid == IID_ITfTextInputProcessorEx) {
    *ppvObj = static_cast<ITfTextInputProcessorEx*>(this);
  } else if (riid == IID_ITfKeyEventSink) {
    *ppvObj = static_cast<ITfKeyEventSink*>(this);
  } else if (riid == IID_ITfThreadMgrEventSink) {
    *ppvObj = static_cast<ITfThreadMgrEventSink*>(this);
  } else if (riid == IID_ITfCompositionSink) {
    *ppvObj = static_cast<ITfCompositionSink*>(this);
  } else if (riid == IID_ITfDisplayAttributeProvider) {
    *ppvObj = static_cast<ITfDisplayAttributeProvider*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) TextService::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&ref_count_)); }
STDMETHODIMP_(ULONG) TextService::Release() {
  const auto c = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  if (c == 0) delete this;
  return c;
}

STDMETHODIMP TextService::Activate(ITfThreadMgr* ptim, TfClientId tid) { return ActivateEx(ptim, tid, 0); }

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) {
  // dwFlags does not officially enumerate the UIElement bit, so UI-less state
  // is taken from ITfThreadMgrEx::GetActiveFlags instead (spec §2.10).
  UNREFERENCED_PARAMETER(dwFlags);
  if (!ptim) return E_INVALIDARG;
  if (thread_mgr_) return E_UNEXPECTED;

  thread_mgr_ = ptim;
  client_id_ = tid;
  thread_mgr_->AddRef();

  // Detect UI-less mode (Windows 11 / Office). ITfThreadMgrEx may be absent on
  // older hosts; default to false and treat any failure as "not UI-less".
  ui_less_mode_ = false;
  {
    ITfThreadMgrEx* thread_mgr_ex = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfThreadMgrEx,
                                              reinterpret_cast<void**>(&thread_mgr_ex))) &&
        thread_mgr_ex) {
      DWORD active_flags = 0;
      if (SUCCEEDED(thread_mgr_ex->GetActiveFlags(&active_flags)))
        ui_less_mode_ = (active_flags & TF_TMF_UIELEMENTENABLEDONLY) != 0;
      thread_mgr_ex->Release();
    }
  }

  HRESULT hr = AdviseTextServiceSinks();
  if (FAILED(hr)) {
    DebugLog("ActivateEx: failed to advise TSF sinks");
    UnadviseTextServiceSinks();
    thread_mgr_->Release();
    thread_mgr_ = nullptr;
    client_id_ = TF_CLIENTID_NULL;
    return hr;
  }

  candidate_window_.Create();
  candidate_window_.SetOnClick([this](int idx) {
    selected_candidate_idx_ = idx;
    if (active_context_) CommitSelected(active_context_);
  });
  candidate_window_.SetOnCandidatesReady(&TextService::OnCandidatesReady, this);

  StartIpcWorker();
  return S_OK;
}

STDMETHODIMP TextService::Deactivate() {
  HRESULT result = UnadviseTextServiceSinks();

  StopIpcWorker();

  CleanupForLifecycleLoss(active_context_, /*release_active_context=*/false,
                          LifecycleCleanupFailurePolicy::ReleaseComposition);
  candidate_window_.Destroy();

  if (active_context_) {
    active_context_->Release();
    active_context_ = nullptr;
  }
  if (thread_mgr_) {
    thread_mgr_->Release();
    thread_mgr_ = nullptr;
  }
  client_id_ = TF_CLIENTID_NULL;
  return result;
}

HRESULT TextService::AdviseTextServiceSinks() {
  if (!thread_mgr_) return E_UNEXPECTED;

  ITfKeystrokeMgr* key_mgr = nullptr;
  HRESULT hr = thread_mgr_->QueryInterface(IID_ITfKeystrokeMgr,
                                           reinterpret_cast<void**>(&key_mgr));
  if (FAILED(hr) || !key_mgr) return FAILED(hr) ? hr : E_NOINTERFACE;

  hr = key_mgr->AdviseKeyEventSink(client_id_, this, TRUE);
  key_mgr->Release();
  if (FAILED(hr)) return hr;
  key_event_sink_advised_ = true;

  ITfSource* source = nullptr;
  hr = thread_mgr_->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source));
  if (FAILED(hr) || !source) return FAILED(hr) ? hr : E_NOINTERFACE;

  hr = source->AdviseSink(IID_ITfThreadMgrEventSink,
                          static_cast<ITfThreadMgrEventSink*>(this),
                          &thread_mgr_sink_cookie_);
  source->Release();
  if (FAILED(hr)) return hr;
  return S_OK;
}

HRESULT TextService::UnadviseTextServiceSinks() {
  HRESULT result = S_OK;

  if (thread_mgr_ && thread_mgr_sink_cookie_ != TF_INVALID_COOKIE) {
    ITfSource* source = nullptr;
    HRESULT hr = thread_mgr_->QueryInterface(IID_ITfSource,
                                             reinterpret_cast<void**>(&source));
    if (SUCCEEDED(hr) && source) {
      hr = source->UnadviseSink(thread_mgr_sink_cookie_);
      source->Release();
    }
    if (FAILED(hr)) {
      DebugLog("Deactivate: failed to unadvise thread manager sink");
      result = hr;
    } else {
      thread_mgr_sink_cookie_ = TF_INVALID_COOKIE;
    }
  }

  if (thread_mgr_ && key_event_sink_advised_) {
    ITfKeystrokeMgr* key_mgr = nullptr;
    HRESULT hr = thread_mgr_->QueryInterface(IID_ITfKeystrokeMgr,
                                             reinterpret_cast<void**>(&key_mgr));
    if (SUCCEEDED(hr) && key_mgr) {
      hr = key_mgr->UnadviseKeyEventSink(client_id_);
      key_mgr->Release();
    }
    if (FAILED(hr)) {
      DebugLog("Deactivate: failed to unadvise key event sink");
      if (SUCCEEDED(result)) result = hr;
    } else {
      key_event_sink_advised_ = false;
    }
  }

  return result;
}

STDMETHODIMP TextService::OnSetFocus(BOOL foreground) {
  if (!foreground) {
    CleanupForLifecycleLoss(active_context_, /*release_active_context=*/true,
                            LifecycleCleanupFailurePolicy::PreserveComposition);
  }
  return S_OK;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
  UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(lParam);
  if (!eaten) return E_INVALIDARG;
  *eaten = FALSE;

  if (HasSystemModifierDown()) return S_OK;

  const bool has_preedit = !preedit_kana_.empty() || romaji_.HasPending();
  const bool cand_visible = candidate_window_.IsVisible();

  if (wParam >= 'A' && wParam <= 'Z') {
    *eaten = TRUE;
  } else if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) {
    // 長音: ハイフンキー（主キー・テンキー）は composition 中のみ長音符「ー」として取り込む。
    // composition が無いときは通常のハイフンとしてアプリへ通す。
    *eaten = has_preedit ? TRUE : FALSE;
  } else if (wParam == VK_BACK) {
    *eaten = has_preedit ? TRUE : FALSE;
  } else if (wParam == VK_SPACE) {
    *eaten = (has_preedit || cand_visible) ? TRUE : FALSE;
  } else if (wParam == VK_UP || wParam == VK_DOWN) {
    *eaten = cand_visible ? TRUE : FALSE;
  } else if (wParam == VK_RETURN) {
    *eaten = (cand_visible || has_preedit) ? TRUE : FALSE;
  } else if (wParam == VK_ESCAPE) {
    *eaten = (cand_visible || has_preedit) ? TRUE : FALSE;
  } else if (wParam >= '1' && wParam <= '9') {
    *eaten = cand_visible ? TRUE : FALSE;
  }
  return S_OK;
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
  UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(wParam); UNREFERENCED_PARAMETER(lParam);
  if (!eaten) return E_INVALIDARG;
  *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
  UNREFERENCED_PARAMETER(lParam);
  if (!eaten) return E_INVALIDARG;
  *eaten = FALSE;
  try {
    if (HasSystemModifierDown()) return S_OK;

    const bool cand_visible = candidate_window_.IsVisible();

    if (wParam >= 'A' && wParam <= 'Z') {
      // Hide candidate window when the user resumes typing.
      if (cand_visible) {
        candidate_window_.Hide();
        selected_candidate_idx_ = 0;
      }
      // Always clear stale candidates when the reading changes, not only when
      // the window is visible — otherwise a Space press before the fresh
      // QueryCandidates response arrives snapshots the old cache.
      {
        std::lock_guard<std::mutex> lk(candidates_mtx_);
        candidates_.clear();
        candidate_window_show_pending_ = false;
      }
      preedit_kana_ += romaji_.Feed(static_cast<char>(wParam));
      RequestPreeditUpdate(context);
      PostQueryCandidates(preedit_kana_);
      *eaten = TRUE;

    } else if ((wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) &&
               (!preedit_kana_.empty() || romaji_.HasPending())) {
      // 長音: composition 中のハイフンキー（主キー・テンキー）を長音符「ー」として
      // preedit に取り込む。composition が無いときは本分岐に入らず、ハイフンはアプリへ
      // パススルーする。
      if (cand_visible) {
        candidate_window_.Hide();
        selected_candidate_idx_ = 0;
      }
      {
        std::lock_guard<std::mutex> lk(candidates_mtx_);
        candidates_.clear();
        candidate_window_show_pending_ = false;
      }
      preedit_kana_ += romaji_.Feed('-');
      RequestPreeditUpdate(context);
      PostQueryCandidates(preedit_kana_);
      *eaten = TRUE;

    } else if (wParam == VK_BACK) {
      if (cand_visible) {
        candidate_window_.Hide();
        selected_candidate_idx_ = 0;
      }
      if (romaji_.HasPending()) {
        romaji_.PopPending();
        *eaten = TRUE;
      } else if (!preedit_kana_.empty()) {
        {
          std::lock_guard<std::mutex> lk(candidates_mtx_);
          candidates_.clear();
          candidate_window_show_pending_ = false;
        }
        auto& s = preedit_kana_;
        size_t i = s.size();
        while (i > 0 && (s[i - 1] & 0xC0) == 0x80) --i;
        if (i > 0) --i;
        s.erase(i);
        RequestPreeditUpdate(context);
        if (!preedit_kana_.empty()) PostQueryCandidates(preedit_kana_);
        *eaten = TRUE;
      }

    } else if (wParam == VK_SPACE) {
      // Flush any pending romaji so the reading is complete.
      const std::string flushed = romaji_.Flush();
      if (!flushed.empty()) {
        preedit_kana_ += flushed;
        RequestPreeditUpdate(context);
        // Reading changed due to romaji flush; old candidates are now stale.
        // Clear them and query for the updated reading so the candidate window
        // (opened below) reflects the complete input, not the pre-flush state.
        {
          std::lock_guard<std::mutex> lk(candidates_mtx_);
          candidates_.clear();
          candidate_window_show_pending_ = false;
        }
        PostQueryCandidates(preedit_kana_);
      }
      if (!preedit_kana_.empty()) {
        // Always eat Space during preedit — even if candidates haven't arrived
        // yet — to prevent a literal space leaking into the application.
        *eaten = TRUE;
        if (cand_visible) {
          // Cycle to next candidate using the existing snapshot.
          candidate_window_.MoveSelection(+1);
          selected_candidate_idx_ = candidate_window_.GetSelected();
        } else {
          // Snapshot the candidate list so commit always reflects what was
          // displayed, even if a late QueryCandidates response arrives later.
          std::vector<ipc::CandidateField> snapshot;
          bool wait_for_candidates = false;
          {
            std::lock_guard<std::mutex> lk(candidates_mtx_);
            if (candidates_.empty()) {
              candidate_window_show_pending_ = true;
              wait_for_candidates = true;
            } else {
              candidate_window_show_pending_ = false;
              shown_candidates_ = candidates_;
              snapshot = shown_candidates_;
            }
          }
          if (!wait_for_candidates) {
            std::vector<std::wstring> items;
            for (const auto& c : snapshot) items.push_back(Utf8ToWide(c.surface));
            if (!items.empty()) {
              selected_candidate_idx_ = 0;
              POINT pt = caret_pt_;
              if (pt.x == 0 && pt.y == 0) GetCursorPos(&pt);
              candidate_window_.Show(pt, items, 0);
            }
          }
        }
      }

    } else if (wParam == VK_UP) {
      if (cand_visible) {
        candidate_window_.MoveSelection(-1);
        selected_candidate_idx_ = candidate_window_.GetSelected();
        *eaten = TRUE;
      }

    } else if (wParam == VK_DOWN) {
      if (cand_visible) {
        candidate_window_.MoveSelection(+1);
        selected_candidate_idx_ = candidate_window_.GetSelected();
        *eaten = TRUE;
      }

    } else if (wParam == VK_RETURN) {
      if (cand_visible) {
        CommitSelected(context);
        *eaten = TRUE;
      } else if (!preedit_kana_.empty() || romaji_.HasPending()) {
        CommitPreeditAsIs(context);
        *eaten = TRUE;
      }

    } else if (wParam >= '1' && wParam <= '9') {
      if (cand_visible) {
        *eaten = TRUE;
        int idx = static_cast<int>(wParam - '1');
        if (idx < candidate_window_.GetCount()) {
          selected_candidate_idx_ = idx;
          CommitSelected(context);
        }
      }

    } else if (wParam == VK_ESCAPE) {
      if (cand_visible) {
        candidate_window_.Hide();
        selected_candidate_idx_ = 0;
        *eaten = TRUE;
      } else if (!preedit_kana_.empty() || romaji_.HasPending()) {
        preedit_kana_.clear();
        romaji_.Reset();
        {
          std::lock_guard<std::mutex> lk(candidates_mtx_);
          candidates_.clear();
          candidate_window_show_pending_ = false;
        }
        RequestPreeditUpdate(context);
        *eaten = TRUE;
      }
    }
  } catch (...) {
    return E_FAIL;
  }
  return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
  UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(wParam); UNREFERENCED_PARAMETER(lParam);
  if (!eaten) return E_INVALIDARG;
  *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext* context, REFGUID rguid, BOOL* eaten) {
  UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(rguid);
  if (!eaten) return E_INVALIDARG;
  *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr* pdim) { UNREFERENCED_PARAMETER(pdim); return S_OK; }
STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr* pdim) {
  if (ActiveContextBelongsToDocumentMgr(pdim)) {
    CleanupForLifecycleLoss(active_context_, /*release_active_context=*/true,
                            LifecycleCleanupFailurePolicy::ReleaseComposition);
  }
  return S_OK;
}
STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus) {
  if (!SameComIdentity(pdimFocus, pdimPrevFocus)) {
    CleanupForLifecycleLoss(active_context_, /*release_active_context=*/true,
                            LifecycleCleanupFailurePolicy::PreserveComposition);
  }
  return S_OK;
}
STDMETHODIMP TextService::OnPushContext(ITfContext* pic) { UNREFERENCED_PARAMETER(pic); return S_OK; }
STDMETHODIMP TextService::OnPopContext(ITfContext* pic) {
  if (pic && active_context_ && SameComIdentity(pic, active_context_)) {
    CleanupForLifecycleLoss(pic, /*release_active_context=*/true,
                            LifecycleCleanupFailurePolicy::PreserveComposition);
  }
  return S_OK;
}

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie /*ecWrite*/, ITfComposition* pComposition) {
  if (composition_ == pComposition) {
    composition_->Release();
    composition_ = nullptr;
  }
  ClearCandidateStateForLifecycle();
  CancelPendingQueriesForLifecycle();
  const bool preserve_pending_commit = committing_;
  const std::string pending_commit_surface = commit_surface_;
  ClearTextStateForLifecycle();
  if (preserve_pending_commit) {
    committing_ = true;
    commit_surface_ = pending_commit_surface;
  }
  return S_OK;
}

STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) {
  if (!ppEnum) return E_INVALIDARG;
  *ppEnum = new azookey::tsf::EnumDisplayAttributeInfo();
  return S_OK;
}

STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID guidInfo, ITfDisplayAttributeInfo** ppInfo) {
  if (!ppInfo) return E_INVALIDARG;
  *ppInfo = nullptr;
  if (IsEqualGUID(guidInfo, kInputAttributeGuid)) {
    *ppInfo = new InputDisplayAttributeInfo();
    return S_OK;
  }
  return E_INVALIDARG;
}

HRESULT TextService::RequestPreeditUpdate(ITfContext* context) {
  if (!context) return E_INVALIDARG;
  if (active_context_ != context) {
    if (active_context_) active_context_->Release();
    active_context_ = context;
    active_context_->AddRef();
  }
  ITfEditSession* edit = new EditSession(this, context);
  HRESULT hr_session = S_OK;
  HRESULT hr = context->RequestEditSession(client_id_, edit, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr_session);
  edit->Release();
  // For synchronous sessions TSF writes DoEditSession's result into hr_session
  // while returning S_OK from RequestEditSession itself.  Return hr_session so
  // callers see real failures.  For async sessions hr_session stays S_OK
  // (initialized value), so this is always safe to return.
  return SUCCEEDED(hr) ? hr_session : hr;
}

void TextService::ClearCandidateStateForLifecycle() {
  candidate_window_.Hide();
  selected_candidate_idx_ = 0;
  shown_candidates_.clear();
  {
    std::lock_guard<std::mutex> lk(candidates_mtx_);
    candidates_.clear();
    candidate_window_show_pending_ = false;
  }
}

void TextService::CancelPendingQueriesForLifecycle() {
  std::lock_guard<std::mutex> lk(ipc_mtx_);
  const bool can_send_cancel = ipc_thread_.joinable() && !ipc_stop_.load();
  bool need_notify = false;
  if (ipc_has_request_) {
    if (can_send_cancel) {
      ipc_send_queue_.push_back(
          {ipc::MessageType::Cancel, ipc::BuildCancel({ipc_pending_id_}), false,
           ipc_pending_id_});
      need_notify = true;
    }
    ipc_has_request_ = false;
  }
  if (can_send_cancel && ipc_inflight_id_ != 0) {
    ipc_send_queue_.push_back(
        {ipc::MessageType::Cancel, ipc::BuildCancel({ipc_inflight_id_}), false,
         ipc_inflight_id_});
    need_notify = true;
  }
  ++ipc_pending_id_;
  if (need_notify) ipc_cv_.notify_one();
}

void TextService::ClearTextStateForLifecycle() {
  preedit_kana_.clear();
  romaji_.Reset();
  committing_ = false;
  commit_surface_.clear();
}

bool TextService::ActiveContextBelongsToDocumentMgr(ITfDocumentMgr* document_mgr) const {
  if (!document_mgr || !active_context_) return false;

  ITfDocumentMgr* active_document_mgr = nullptr;
  if (SUCCEEDED(active_context_->GetDocumentMgr(&active_document_mgr)) && active_document_mgr) {
    const bool owns_active_context = SameComIdentity(active_document_mgr, document_mgr);
    active_document_mgr->Release();
    if (owns_active_context) return true;
  }

  ITfContext* context = nullptr;
  if (SUCCEEDED(document_mgr->GetTop(&context)) && context) {
    const bool is_active_context = SameComIdentity(context, active_context_);
    context->Release();
    if (is_active_context) return true;
  }
  context = nullptr;
  if (SUCCEEDED(document_mgr->GetBase(&context)) && context) {
    const bool is_active_context = SameComIdentity(context, active_context_);
    context->Release();
    if (is_active_context) return true;
  }
  return false;
}

bool TextService::RequestLifecycleCommitOrEndComposition(ITfContext* context) {
  std::string pending_surface;
  if (committing_) {
    pending_surface = commit_surface_;
  } else {
    pending_surface = preedit_kana_;
    if (romaji_.HasPending()) {
      auto romaji_preview = romaji_;
      pending_surface += romaji_preview.Flush();
    }
  }
  const bool has_lifecycle_commit_surface = committing_ || !pending_surface.empty();
  if (!composition_ && !has_lifecycle_commit_surface) return true;
  if (!context) return false;

  const std::string saved_preedit = preedit_kana_;
  const bool saved_committing = committing_;
  const std::string saved_commit_surface = commit_surface_;

  const bool commit_without_composition = composition_ == nullptr;
  if (has_lifecycle_commit_surface) {
    committing_ = true;
    commit_surface_ = pending_surface;
    preedit_kana_.clear();
  } else {
    // The empty-preedit edit session ends the TSF composition without replacing
    // its range text, so focus loss/Deactivate auto-commits the current preedit
    // as specified in docs/tsf-deep-integration-spec.md §4.3.
    preedit_kana_.clear();
    committing_ = false;
    commit_surface_.clear();
  }

  ITfEditSession* edit = new EditSession(this, context);
  HRESULT hr_session = E_FAIL;
  const HRESULT hr =
      context->RequestEditSession(client_id_, edit, TF_ES_SYNC | TF_ES_READWRITE, &hr_session);
  edit->Release();

  const bool completed =
      SUCCEEDED(hr) && SUCCEEDED(hr_session) &&
      (commit_without_composition ? (!committing_ && commit_surface_.empty())
                                  : (composition_ == nullptr));
  if (!completed) {
    preedit_kana_ = saved_preedit;
    committing_ = saved_committing;
    commit_surface_ = saved_commit_surface;
    DebugLog("Lifecycle cleanup: commit/end edit session was rejected or incomplete");
  }
  return completed;
}

void TextService::CleanupForLifecycleLoss(ITfContext* context,
                                          bool release_active_context,
                                          LifecycleCleanupFailurePolicy failure_policy) {
  ClearCandidateStateForLifecycle();
  CancelPendingQueriesForLifecycle();

  ITfContext* cleanup_context = context ? context : active_context_;
  const bool lifecycle_text_committed = RequestLifecycleCommitOrEndComposition(cleanup_context);
  if (lifecycle_text_committed) {
    ClearTextStateForLifecycle();
  } else {
    if (failure_policy == LifecycleCleanupFailurePolicy::PreserveComposition) {
      // Focus/context loss can race with transient TSF lock denial. Keep the
      // active context/composition so a later accepted session or termination
      // callback can finish the lifecycle cleanup.
      return;
    }
    if (composition_) {
      composition_->Release();
      composition_ = nullptr;
    }
    ClearTextStateForLifecycle();
  }

  if (release_active_context &&
      (!cleanup_context || SameComIdentity(cleanup_context, active_context_))) {
    if (active_context_) {
      active_context_->Release();
      active_context_ = nullptr;
    }
  }
}

// --- Commit helpers (M5) ---

void TextService::CommitSelected(ITfContext* context) {
  if (!context) return;

  // Use the snapshot taken when Space opened the window so that a late
  // QueryCandidates response cannot silently alter what gets committed.
  ipc::CandidateField chosen;
  std::vector<ipc::CandidateField> shown = shown_candidates_;
  if (!shown_candidates_.empty() &&
      selected_candidate_idx_ >= 0 &&
      selected_candidate_idx_ < static_cast<int>(shown_candidates_.size())) {
    chosen = shown_candidates_[selected_candidate_idx_];
  }
  shown_candidates_.clear();
  {
    std::lock_guard<std::mutex> lk(candidates_mtx_);
    candidates_.clear();
    candidate_window_show_pending_ = false;
  }
  const std::string reading = preedit_kana_;

  candidate_window_.Hide();
  selected_candidate_idx_ = 0;

  // M10: cancel queued AND in-flight QC so the host can abort early.
  // Also bump ipc_pending_id_ so any response already en-route fails the
  // staleness check (req_id == ipc_pending_id_) even if no new request is
  // queued after this commit.
  {
    std::lock_guard<std::mutex> lk(ipc_mtx_);
    bool need_notify = false;
    if (ipc_has_request_) {
      ipc_send_queue_.push_back(
          {ipc::MessageType::Cancel, ipc::BuildCancel({ipc_pending_id_}), false,
           ipc_pending_id_});
      ipc_has_request_ = false;
      need_notify = true;
    }
    if (ipc_inflight_id_ != 0) {
      ipc_send_queue_.push_back(
          {ipc::MessageType::Cancel, ipc::BuildCancel({ipc_inflight_id_}), false,
           ipc_inflight_id_});
      need_notify = true;
    }
    ++ipc_pending_id_;
    if (need_notify) ipc_cv_.notify_one();
  }

  commit_surface_ = chosen.surface.empty() ? preedit_kana_ : chosen.surface;
  committing_ = true;
  // Defer clearing preedit until the session is accepted so that if TSF
  // rejects the request (lock denial, context teardown) the composition text
  // is not lost and the user can still see/retry their input.
  const bool session_ok = SUCCEEDED(RequestPreeditUpdate(context));
  if (session_ok) {
    preedit_kana_.clear();
    romaji_.Reset();
  } else {
    committing_ = false;
    commit_surface_.clear();
  }

  // Only record the learning event when the edit session was accepted; an
  // observation for a commit that never happened would skew ranking data.
  if (session_ok && !chosen.surface.empty() && !reading.empty()) {
    PostCommitObservation(reading, chosen, shown);
  }
}

void TextService::CommitPreeditAsIs(ITfContext* context) {
  if (!context) return;

  // Flush any pending romaji first.
  const std::string flushed = romaji_.Flush();
  preedit_kana_ += flushed;

  if (preedit_kana_.empty()) return;

  candidate_window_.Hide();
  selected_candidate_idx_ = 0;
  {
    std::lock_guard<std::mutex> lk(candidates_mtx_);
    candidates_.clear();
    candidate_window_show_pending_ = false;
  }

  // M10: cancel queued AND in-flight QC; bump pending_id to invalidate
  // any response already en-route (mirrors CommitSelected logic).
  {
    std::lock_guard<std::mutex> lk(ipc_mtx_);
    bool need_notify = false;
    if (ipc_has_request_) {
      ipc_send_queue_.push_back(
          {ipc::MessageType::Cancel, ipc::BuildCancel({ipc_pending_id_}), false,
           ipc_pending_id_});
      ipc_has_request_ = false;
      need_notify = true;
    }
    if (ipc_inflight_id_ != 0) {
      ipc_send_queue_.push_back(
          {ipc::MessageType::Cancel, ipc::BuildCancel({ipc_inflight_id_}), false,
           ipc_inflight_id_});
      need_notify = true;
    }
    ++ipc_pending_id_;
    if (need_notify) ipc_cv_.notify_one();
  }

  commit_surface_ = preedit_kana_;
  committing_ = true;
  // Same deferred-clear pattern as CommitSelected: preserve preedit until the
  // session is accepted so rejection does not silently discard the reading.
  if (SUCCEEDED(RequestPreeditUpdate(context))) {
    preedit_kana_.clear();
    romaji_.Reset();
  } else {
    committing_ = false;
    commit_surface_.clear();
  }
}

// --- IPC worker (M4 + M6 + M10) ---

void TextService::StartIpcWorker() {
  ipc_stop_.store(false);
  ipc_thread_ = std::thread(&TextService::IpcWorkerThread, this);
}

void TextService::StopIpcWorker() {
  if (!ipc_thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    ipc_stop_.store(true);
  }
  ipc_cv_.notify_one();
  ipc_client_.Disconnect();
  ipc_thread_.join();
}

// Interruptible backoff between reconnect attempts. Returns true when the
// worker should stop (Deactivate set ipc_stop_); StopIpcWorker notifies
// ipc_cv_ so this wakes promptly instead of sleeping out the full delay.
bool TextService::WaitForReconnectOrStop(uint32_t delay_ms) {
  std::unique_lock<std::mutex> lock(ipc_mtx_);
  ipc_cv_.wait_for(lock, std::chrono::milliseconds(delay_ms),
                   [this] { return ipc_stop_.load(); });
  return ipc_stop_.load();
}

// Send the activation handshake and wait (bounded) for acceptance. The bounded
// wait prevents a host that accepts the pipe but never replies from hanging the
// reconnect loop. Returns false on send failure, timeout, or rejection.
bool TextService::PerformHandshake() {
  using namespace azookey::ipc;

  HandshakeRequest hs;
  hs.tip_version = kTipVersion;
  hs.protocol_version = 1;
  hs.capabilities = {"ping", "query_candidates", "commit_observation", "cancel"};
  hs.handshake_token = IpcHandshakeTokenFromEnv();

  Envelope henv;
  henv.version = 1;
  henv.request_id = 1;
  henv.trace_id = "tip-activate-handshake";
  henv.type = MessageType::Handshake;
  henv.payload_json = BuildHandshakeRequest(hs);

  if (!ipc_client_.Send(henv)) {
    DebugLog("IPC: handshake send failed");
    return false;
  }
  constexpr uint32_t kHandshakeTimeoutMs = 3000;
  auto hres = ipc_client_.ReceiveWithTimeout(kHandshakeTimeoutMs);
  auto hpayload = hres ? ParseHandshakeResponse(hres->payload_json) : std::nullopt;
  if (!hpayload || !hpayload->accepted) {
    DebugLog("IPC: handshake rejected or no response");
    return false;
  }
  DebugLog("IPC: connected to host " + hpayload->host_version);
  return true;
}

// Re-arm a QueryCandidates that was pulled from the queue but not delivered
// because the pipe dropped, so the reconnected serve loop re-issues it without
// the user retyping. Skipped when a newer request or a commit already
// superseded it (ipc_pending_id_ has moved on).
void TextService::RearmPendingQuery(uint64_t req_id) {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  if (ipc_pending_id_ == req_id) ipc_has_request_ = true;
}

void TextService::IpcWorkerThread() {
  using namespace azookey::ipc;

  const auto pipe_name = DefaultPipeName();
  if (pipe_name.empty()) {
    DebugLog("IPC: default pipe name unavailable; current-user SID lookup failed");
    return;
  }
  // Reconnect with exponential backoff so the worker survives a host that is
  // started after the TIP, crashes, or restarts. The thread exits only on
  // Deactivate (ipc_stop_), never on a dropped connection (DEV-168).
  constexpr uint32_t kConnectTimeoutMs = 500;
  constexpr uint32_t kBackoffMinMs = 250;
  constexpr uint32_t kBackoffMaxMs = 3000;
  uint32_t backoff_ms = kBackoffMinMs;

  while (!ipc_stop_.load()) {
    const bool established =
        ipc_client_.Connect(pipe_name, kConnectTimeoutMs) && PerformHandshake();
    if (!established) {
      ipc_client_.Disconnect();
      if (WaitForReconnectOrStop(backoff_ms)) break;
      backoff_ms *= 2;
      if (backoff_ms > kBackoffMaxMs) backoff_ms = kBackoffMaxMs;
      continue;
    }

    // Healthy connection: reset backoff and serve until the pipe drops. A
    // QueryCandidates enqueued while the host was down is still pending and is
    // picked up immediately, so candidates recover without the user retyping.
    backoff_ms = kBackoffMinMs;
    ServeConnection();

    ipc_client_.Disconnect();
    {
      std::lock_guard<std::mutex> lock(ipc_mtx_);
      ipc_inflight_id_ = 0;
    }
  }

  DebugLog("IPC: worker exiting");
}

// Serve QueryCandidates / fire-and-forget traffic over an already-handshaken
// connection. Returns when the pipe drops (so the caller reconnects) or when
// Deactivate sets ipc_stop_. All M10 cancel / staleness handling is unchanged.
void TextService::ServeConnection() {
  using namespace azookey::ipc;

  uint64_t next_id = 2;

  while (true) {
    std::string reading;
    uint64_t req_id = 0;
    bool has_qc = false;
    std::vector<IpcSendItem> to_send;

    {
      std::unique_lock<std::mutex> lock(ipc_mtx_);
      ipc_cv_.wait(lock, [this] {
        return ipc_stop_.load() || ipc_has_request_ || !ipc_send_queue_.empty();
      });
      if (ipc_stop_.load()) break;

      to_send = std::move(ipc_send_queue_);
      ipc_send_queue_.clear();

      if (ipc_has_request_) {
        reading = ipc_pending_reading_;
        req_id = ipc_pending_id_;
        ipc_has_request_ = false;
        has_qc = true;
      }
    }

    // Drain fire-and-forget queue (CommitObservation, Cancel) first. A send or
    // response failure here means the pipe dropped (e.g. the host restarted):
    // re-arm any query pulled this iteration and return so the outer loop
    // reconnects, instead of looping back to the wait on a dead connection.
    for (auto& item : to_send) {
      Envelope env;
      env.version = 1;
      env.request_id = next_id++;
      env.trace_id = "tip-faf";
      env.type = item.type;
      env.payload_json = item.payload_json;
      if (!ipc_client_.Send(env)) {
        if (!ipc_stop_.load())
          DebugLog("IPC: faf send failed for type=" + TypeToString(item.type) +
                   "; reconnecting");
        if (has_qc) RearmPendingQuery(req_id);
        return;
      }
      if (item.expects_response) {
        auto res = ipc_client_.Receive();
        if (!res) {
          if (!ipc_stop_.load())
            DebugLog("IPC: faf receive failed for type=" + TypeToString(item.type) +
                     "; reconnecting");
          if (has_qc) RearmPendingQuery(req_id);
          return;
        }
      }
    }

    if (!has_qc || reading.empty()) continue;

    QueryCandidatesRequest qreq;
    qreq.reading = reading;
    qreq.left_context = "";
    qreq.max_candidates = 9;
    qreq.live = true;

    Envelope qenv;
    qenv.version = 1;
    qenv.request_id = req_id;
    qenv.trace_id = "tip-key-query";
    qenv.type = MessageType::QueryCandidates;
    qenv.payload_json = BuildQueryCandidatesRequest(qreq);

    // Mark the request as in-flight so CommitSelected can cancel it even
    // after ipc_has_request_ has been cleared by this thread.
    {
      std::lock_guard<std::mutex> lock(ipc_mtx_);
      ipc_inflight_id_ = req_id;
    }

    if (!ipc_client_.Send(qenv)) {
      {
        std::lock_guard<std::mutex> lock(ipc_mtx_);
        ipc_inflight_id_ = 0;
      }
      // Pipe died after we cleared ipc_has_request_: re-arm so the reconnected
      // loop re-issues this reading without the user retyping.
      RearmPendingQuery(req_id);
      if (!ipc_stop_.load())
        DebugLog("IPC: QueryCandidates send failed; will retry after reconnect");
      break;
    }

    // If a Cancel targeting our just-sent QC arrives, the host returns no
    // response (Dispatcher::HandleQueryCandidates returns std::nullopt for
    // canceled requests).  Track that so we abandon the receive instead of
    // spinning until the pipe disconnects.
    bool cancel_inflight = false;

    // Drain fire-and-forget items (Cancel) that were enqueued while we were
    // preparing/sending this query.  Only items that do NOT expect a response
    // are sent here — response-awaiting items (CommitObservation) stay in the
    // queue for the next loop iteration so their response isn't confused with
    // the pending QueryCandidates response.  Sending cancels here rather than
    // after Receive() means they reach the host pipeline as soon as possible.
    {
      std::vector<IpcSendItem> faf_now;
      {
        std::lock_guard<std::mutex> lock(ipc_mtx_);
        std::vector<IpcSendItem> deferred;
        for (auto& item : ipc_send_queue_) {
          if (!item.expects_response)
            faf_now.push_back(std::move(item));
          else
            deferred.push_back(std::move(item));
        }
        ipc_send_queue_ = std::move(deferred);
      }
      for (auto& item : faf_now) {
        Envelope env;
        env.version = 1;
        env.request_id = next_id++;
        env.trace_id = "tip-faf";
        env.type = item.type;
        env.payload_json = item.payload_json;
        if (!ipc_client_.Send(env))
          DebugLog("IPC: post-qc cancel send failed for type=" + TypeToString(item.type));
        if (item.type == MessageType::Cancel && item.cancel_target_id == req_id)
          cancel_inflight = true;
      }
    }

    // Poll in 50ms slices so cancels enqueued while the host processes the
    // query can be forwarded without waiting for the full response.
    std::optional<ipc::Envelope> qres;
    while (!ipc_stop_.load() && ipc_client_.IsConnected()) {
      qres = ipc_client_.ReceiveWithTimeout(50);
      if (qres) break;
      // Drain fire-and-forget items (Cancel) that arrived while we waited.
      std::vector<IpcSendItem> mid_faf;
      {
        std::lock_guard<std::mutex> lk(ipc_mtx_);
        std::vector<IpcSendItem> deferred;
        for (auto& item : ipc_send_queue_) {
          if (!item.expects_response)
            mid_faf.push_back(std::move(item));
          else
            deferred.push_back(std::move(item));
        }
        ipc_send_queue_ = std::move(deferred);
      }
      for (auto& item : mid_faf) {
        ipc::Envelope env;
        env.version = 1;
        env.request_id = next_id++;
        env.trace_id = "tip-faf-mid";
        env.type = item.type;
        env.payload_json = item.payload_json;
        if (!ipc_client_.Send(env))
          DebugLog("IPC: mid-receive cancel send failed for type=" +
                   ipc::TypeToString(item.type));
        if (item.type == ipc::MessageType::Cancel && item.cancel_target_id == req_id)
          cancel_inflight = true;
      }
    }
    {
      std::lock_guard<std::mutex> lock(ipc_mtx_);
      ipc_inflight_id_ = 0;
    }
    if (!qres) {
      // Host died after the query was sent: re-arm so the reconnected loop
      // re-issues it (recover without retyping).
      RearmPendingQuery(req_id);
      if (!ipc_stop_.load())
        DebugLog("IPC: QueryCandidates receive failed; will retry after reconnect");
      break;
    }

    if (cancel_inflight) {
      // Even when cancel is sent, host processes one request at a time and
      // still returns this query response before seeing Cancel. Consume and
      // discard here to keep stream framing aligned for subsequent receives.
      DebugLog("IPC: in-flight req_id=" + std::to_string(req_id) +
               " canceled, consumed pending query reply");
      ++next_id;
      continue;
    }

    auto qpayload = ParseQueryCandidatesResponse(qres->payload_json);
    if (!qpayload) {
      ++next_id;
      continue;
    }

    // M10: discard stale responses.
    // Conditions for freshness:
    //   1. No newer QC is already queued (ipc_has_request_ false).
    //   2. ipc_pending_id_ still equals req_id — no commit/clear bumped it
    //      while this response was in-transit.
    bool is_fresh = false;
    {
      std::lock_guard<std::mutex> lock(ipc_mtx_);
      is_fresh = !ipc_has_request_ && (req_id == ipc_pending_id_);
    }

    if (is_fresh) {
      if (!qpayload->candidates.empty()) {
        DebugLog("IPC: " + std::to_string(qpayload->candidates.size()) +
                 " candidates for [" + reading + "] top=" + qpayload->candidates[0].surface);
      }
      bool notify_ui = false;
      {
        std::lock_guard<std::mutex> lock(candidates_mtx_);
        candidates_ = qpayload->candidates;
        if (candidate_window_show_pending_) {
          if (candidates_.empty()) {
            candidate_window_show_pending_ = false;
          } else {
            notify_ui = true;
          }
        }
      }
      if (notify_ui) candidate_window_.PostCandidatesReady();
    } else {
      DebugLog("IPC: stale response for req_id=" + std::to_string(req_id) + ", discarding");
    }

    ++next_id;
  }
}

void TextService::PostQueryCandidates(const std::string& reading) {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  ipc_pending_reading_ = reading;
  ++ipc_pending_id_;
  ipc_has_request_ = true;
  ipc_cv_.notify_one();
}

void TextService::OnCandidatesReady(void* context) {
  if (!context) return;
  static_cast<TextService*>(context)->ShowCandidateWindowFromCache();
}

void TextService::ShowCandidateWindowFromCache() {
  if (preedit_kana_.empty() || candidate_window_.IsVisible()) {
    std::lock_guard<std::mutex> lk(candidates_mtx_);
    candidate_window_show_pending_ = false;
    return;
  }

  std::vector<ipc::CandidateField> snapshot;
  {
    std::lock_guard<std::mutex> lk(candidates_mtx_);
    if (!candidate_window_show_pending_) return;
    candidate_window_show_pending_ = false;
    if (candidates_.empty()) return;
    shown_candidates_ = candidates_;
    snapshot = shown_candidates_;
  }

  std::vector<std::wstring> items;
  for (const auto& candidate : snapshot) items.push_back(Utf8ToWide(candidate.surface));
  if (items.empty()) return;

  selected_candidate_idx_ = 0;
  POINT pt = caret_pt_;
  if (pt.x == 0 && pt.y == 0) GetCursorPos(&pt);
  candidate_window_.Show(pt, items, 0);
}

#ifdef AZOOKEY_TSF_TESTING
bool TextService::candidate_window_show_pending_for_test() {
  std::lock_guard<std::mutex> lk(candidates_mtx_);
  return candidate_window_show_pending_;
}

void TextService::set_cached_candidates_for_test(std::vector<ipc::CandidateField> candidates) {
  std::lock_guard<std::mutex> lk(candidates_mtx_);
  candidates_ = std::move(candidates);
}

void TextService::show_candidate_window_from_cache_for_test() {
  ShowCandidateWindowFromCache();
}
#endif

void TextService::PostCommitObservation(const std::string& reading,
                                        const ipc::CandidateField& chosen,
                                        const std::vector<ipc::CandidateField>& shown) {
  using namespace std::chrono;
  const uint64_t now_ms = static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());

  ipc::CommitObservationRequest req;
  req.reading = reading;
  req.chosen = chosen;
  req.shown = shown;
  req.left_context = "";
  req.timestamp_ms = now_ms;

  PostIpcSend(ipc::MessageType::CommitObservation,
              ipc::BuildCommitObservationRequest(req), true);
}

void TextService::PostCancel(uint64_t target_request_id) {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  ipc_send_queue_.push_back(
      {ipc::MessageType::Cancel, ipc::BuildCancel({target_request_id}), false,
       target_request_id});
  ipc_cv_.notify_one();
}

void TextService::PostIpcSend(ipc::MessageType type, std::string payload, bool expects_response) {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  ipc_send_queue_.push_back({type, std::move(payload), expects_response, 0});
  ipc_cv_.notify_one();
}

// --- EditSession ---

EditSession::EditSession(TextService* service, ITfContext* context)
    : service_(service), context_(context) {
  if (context_) context_->AddRef();
}

STDMETHODIMP EditSession::QueryInterface(REFIID riid, void** ppvObj) {
  if (!ppvObj) return E_INVALIDARG;
  *ppvObj = nullptr;
  if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
    *ppvObj = static_cast<ITfEditSession*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) EditSession::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&ref_count_)); }
STDMETHODIMP_(ULONG) EditSession::Release() {
  const auto c = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  if (c == 0) {
    if (context_) context_->Release();
    delete this;
  }
  return c;
}

STDMETHODIMP EditSession::DoEditSession(TfEditCookie ec) {
  // M5/M6: commit path — set final surface text and end composition.
  if (service_->committing_) {
    const std::string pending_commit_surface = service_->commit_surface_;
    service_->committing_ = false;
    const std::wstring surface = Utf8ToWide(pending_commit_surface);
    service_->commit_surface_.clear();
    const auto restore_pending_commit = [&]() {
      service_->committing_ = true;
      service_->commit_surface_ = pending_commit_surface;
    };

    if (service_->composition_) {
      if (!surface.empty()) {
        ITfRange* pRange = nullptr;
        HRESULT text_hr = service_->composition_->GetRange(&pRange);
        if (SUCCEEDED(text_hr) && pRange) {
          text_hr = pRange->SetText(ec, 0, surface.c_str(), static_cast<LONG>(surface.size()));
          pRange->Release();
        }
        if (FAILED(text_hr) || !pRange) {
          restore_pending_commit();
          return FAILED(text_hr) ? text_hr : E_FAIL;
        }
      }
      // EndComposition finalizes the text in the document.
      ITfComposition* comp = service_->composition_;
      comp->AddRef();
      const HRESULT end_hr = comp->EndComposition(ec);
      if (SUCCEEDED(end_hr) && service_->composition_ == comp) {
        service_->composition_ = nullptr;
        comp->Release();
      }
      comp->Release();
      if (FAILED(end_hr)) {
        restore_pending_commit();
        return end_hr;
      }
    } else if (!surface.empty()) {
      // No active composition (e.g. commit triggered before the async preedit
      // session ran); insert the surface text directly at the cursor.
      TF_SELECTION sel{};
      ULONG fetched = 0;
      HRESULT text_hr = E_FAIL;
      if (SUCCEEDED(context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) &&
          fetched > 0 && sel.range) {
        text_hr = sel.range->SetText(ec, 0, surface.c_str(), static_cast<LONG>(surface.size()));
        if (SUCCEEDED(text_hr) && SUCCEEDED(sel.range->Collapse(ec, TF_ANCHOR_END))) {
          context_->SetSelection(ec, 1, &sel);
        }
        sel.range->Release();
      }
      if (FAILED(text_hr)) {
        restore_pending_commit();
        return text_hr;
      }
    }
    return S_OK;
  }

  // Normal preedit update.
  const std::wstring kana = Utf8ToWide(service_->preedit_kana_);

  if (kana.empty()) {
    if (service_->composition_) {
      ITfComposition* comp = service_->composition_;
      service_->composition_ = nullptr;
      comp->EndComposition(ec);
      comp->Release();
    }
    return S_OK;
  }

  // Create composition if not active.
  if (!service_->composition_) {
    ITfContextComposition* pCtxComp = nullptr;
    if (FAILED(context_->QueryInterface(IID_ITfContextComposition,
                                        reinterpret_cast<void**>(&pCtxComp))) ||
        !pCtxComp)
      return E_FAIL;

    TF_SELECTION sel{};
    ULONG fetched = 0;
    context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched);
    if (fetched == 0) {
      pCtxComp->Release();
      return E_FAIL;
    }

    sel.range->Collapse(ec, TF_ANCHOR_END);
    HRESULT hr = pCtxComp->StartComposition(ec, sel.range, service_,
                                             &service_->composition_);
    sel.range->Release();
    pCtxComp->Release();
    if (FAILED(hr) || !service_->composition_) return hr;
  }

  // Update composition text.
  ITfRange* pRange = nullptr;
  if (FAILED(service_->composition_->GetRange(&pRange)) || !pRange) return E_FAIL;

  pRange->SetText(ec, 0, kana.c_str(), static_cast<LONG>(kana.size()));

  // Cache the caret screen position for the candidate window anchor (M5).
  {
    ITfContextView* pView = nullptr;
    if (SUCCEEDED(context_->GetActiveView(&pView)) && pView) {
      RECT rc{};
      BOOL clipped = FALSE;
      if (SUCCEEDED(pView->GetTextExt(ec, pRange, &rc, &clipped))) {
        service_->caret_pt_ = {rc.left, rc.bottom};
      }
      pView->Release();
    }
  }

  // Apply underline display attribute via GUID_PROP_ATTRIBUTE.
  ITfProperty* pProp = nullptr;
  if (SUCCEEDED(context_->GetProperty(GUID_PROP_ATTRIBUTE, &pProp)) && pProp) {
    ITfCategoryMgr* pCatMgr = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITfCategoryMgr, reinterpret_cast<void**>(&pCatMgr))) &&
        pCatMgr) {
      TfGuidAtom atom = TF_INVALID_GUIDATOM;
      pCatMgr->RegisterGUID(kInputAttributeGuid, &atom);
      if (atom != TF_INVALID_GUIDATOM) {
        VARIANT var;
        var.vt = VT_I4;
        var.lVal = static_cast<LONG>(atom);
        pProp->SetValue(ec, pRange, &var);
      }
      pCatMgr->Release();
    }
    pProp->Release();
  }

  pRange->Release();
  return S_OK;
}

}  // namespace azookey::tsf
