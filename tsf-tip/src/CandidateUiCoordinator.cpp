#include "azookey/tsf/CandidateUiCoordinator.h"

#include <algorithm>
#include <new>
#include <utility>

#include "CandidateSelection.h"

namespace azookey::tsf {

#ifdef _DEBUG
#define AZOOKEY_ASSERT_CANDIDATE_UI_THREAD() ui_thread_affinity_.BindOrAssertCurrentThread()
#define AZOOKEY_ASSERT_BOUND_CANDIDATE_UI_THREAD() ui_thread_affinity_.AssertCurrentThreadIfBound()
#else
#define AZOOKEY_ASSERT_CANDIDATE_UI_THREAD() ((void)0)
#define AZOOKEY_ASSERT_BOUND_CANDIDATE_UI_THREAD() ((void)0)
#endif

namespace {

constexpr DWORD kAllInitialCandidateFlags = TF_CLUIE_COUNT | TF_CLUIE_STRING | TF_CLUIE_SELECTION |
                                            TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;

std::vector<std::wstring> CandidateSurfaces(const std::vector<CandidateViewItem>& items) {
  std::vector<std::wstring> surfaces;
  surfaces.reserve(items.size());
  for (const auto& item : items) surfaces.push_back(item.surface);
  return surfaces;
}

}  // namespace

CandidateUiCoordinator::~CandidateUiCoordinator() { Destroy(); }

bool CandidateUiCoordinator::Create() {
  AZOOKEY_ASSERT_CANDIDATE_UI_THREAD();
  return own_window_.Create();
}

void CandidateUiCoordinator::Destroy() {
  AZOOKEY_ASSERT_CANDIDATE_UI_THREAD();
  EndUI();
  own_window_.Destroy();
  ReleaseUiElementMgr();
}

void CandidateUiCoordinator::SetUiLessMode(bool ui_less) {
  AZOOKEY_ASSERT_BOUND_CANDIDATE_UI_THREAD();
  ui_less_mode_ = ui_less;
}

void CandidateUiCoordinator::SetOnClick(CandidateWindow::OnClickFn fn) {
  AZOOKEY_ASSERT_BOUND_CANDIDATE_UI_THREAD();
  own_window_.SetOnClick(std::move(fn));
}

void CandidateUiCoordinator::SetOnRetry(CandidateWindow::OnRetryFn fn) {
  AZOOKEY_ASSERT_BOUND_CANDIDATE_UI_THREAD();
  own_window_.SetOnRetry(std::move(fn));
}

void CandidateUiCoordinator::SetHealthState(CandidateHealthState state,
                                            const std::string& host_generation_id) {
  AZOOKEY_ASSERT_BOUND_CANDIDATE_UI_THREAD();
  const bool changed = state != health_state_;
  const bool new_safe_generation =
      state == CandidateHealthState::SafeMode &&
      std::find(notified_safe_mode_generations_.begin(), notified_safe_mode_generations_.end(),
                host_generation_id) == notified_safe_mode_generations_.end();
  health_state_ = state;
  host_generation_id_ = host_generation_id;
  if (state == CandidateHealthState::Healthy) {
    health_banner_pending_ = false;
    own_window_.ShowHealthBanner(CandidateHealthState::Healthy);
    return;
  }
  if (changed || new_safe_generation) {
    health_banner_pending_ = state != CandidateHealthState::SafeMode || new_safe_generation;
    own_window_.ShowHealthBanner(CandidateHealthState::Healthy);
  }
  ShowPendingHealthBanner();
}

void CandidateUiCoordinator::SetRetryInFlight(bool in_flight) {
  AZOOKEY_ASSERT_BOUND_CANDIDATE_UI_THREAD();
  own_window_.SetRetryInFlight(in_flight);
}

void CandidateUiCoordinator::SetSecureState(bool secure, bool show_indicator) {
  AZOOKEY_ASSERT_BOUND_CANDIDATE_UI_THREAD();
  if (secure == secure_ && show_indicator == show_secure_indicator_) return;
  const bool entered = secure && !secure_;
  secure_ = secure;
  show_secure_indicator_ = show_indicator;
  const bool visible = secure && show_indicator;
  if (entered && visible) secure_toast_pending_ = true;
  if (!visible) {
    secure_toast_pending_ = false;
    own_window_.HideSecureToast();
  }
  own_window_.SetSecureIndicator(visible);
  ShowPendingSecureToast();
}

void CandidateUiCoordinator::SetOnCandidatesReady(CandidateWindow::OnCandidatesReadyFn fn,
                                                  void* context) {
  AZOOKEY_ASSERT_BOUND_CANDIDATE_UI_THREAD();
  own_window_.SetOnCandidatesReady(fn, context);
}

void CandidateUiCoordinator::SetBeginObserver(CandidateUiBeginObserver observer, void* context) {
  AZOOKEY_ASSERT_BOUND_CANDIDATE_UI_THREAD();
  begin_observer_ = observer;
  begin_observer_context_ = context;
}

void CandidateUiCoordinator::PostCandidatesReady() { own_window_.PostCandidatesReady(); }

bool CandidateUiCoordinator::ScheduleCandidatesReady(UINT delay_ms) {
  AZOOKEY_ASSERT_BOUND_CANDIDATE_UI_THREAD();
  return own_window_.ScheduleCandidatesReady(delay_ms);
}

void CandidateUiCoordinator::CancelScheduledCandidatesReady() {
  AZOOKEY_ASSERT_BOUND_CANDIDATE_UI_THREAD();
  own_window_.CancelScheduledCandidatesReady();
}

HRESULT CandidateUiCoordinator::BeginUI(ITfThreadMgr* thread_mgr, POINT pt,
                                        const std::vector<CandidateViewItem>& items,
                                        int selected_idx, std::wstring notice) {
  AZOOKEY_ASSERT_CANDIDATE_UI_THREAD();
  const bool had_ui_element_mgr = ui_element_mgr_ != nullptr;
  const auto rollback_exception = [this, had_ui_element_mgr]() {
    try {
      const HRESULT cleanup_hr = EndUI();
      UNREFERENCED_PARAMETER(cleanup_hr);
    } catch (...) {
      // A misbehaving TSF provider may throw during EndUIElement. Still drop our references.
      ui_element_id_ = kInvalidUiElementId;
      showing_ = false;
      tip_draws_ = true;
      items_.clear();
      notice_.clear();
      selected_idx_ = -1;
      ReleaseUiElement();
    }
    if (!had_ui_element_mgr) ReleaseUiElementMgr();
  };
  try {
    if (items.empty()) return EndUI();

    // Re-showing the list (segment moves, rollbacks) is not closing it, so a
    // secure toast it hides comes back for the rest of its five seconds.
    const bool resume_secure_toast = own_window_.IsSecureToastVisible();
    const HRESULT end_hr = EndUI();
    if (FAILED(end_hr)) {
      NotifyBeginObserver(end_hr, ui_element_mgr_ != nullptr, false, FALSE, kInvalidUiElementId);
      return end_hr;
    }

    items_ = items;
    notice_ = std::move(notice);
    selected_idx_ = ClampSelection(selected_idx);
    last_pt_ = pt;

    auto* element = new (std::nothrow)
        CandidateListUIElement(CandidateSurfaces(items_), selected_idx_, notice_);
    if (!element) {
      NotifyBeginObserver(E_OUTOFMEMORY, ui_element_mgr_ != nullptr, false, FALSE,
                          kInvalidUiElementId);
      return E_OUTOFMEMORY;
    }
    ui_element_.attach(element);
    ui_element_->SetShowCallback([this](bool show) { OnElementShow(show); });

    HRESULT hr = EnsureUiElementMgr(thread_mgr);
    if (FAILED(hr) || !ui_element_mgr_) {
      if (ui_less_mode_) {
        ReleaseUiElement();
        items_.clear();
        notice_.clear();
        selected_idx_ = -1;
        const HRESULT result = FAILED(hr) ? hr : E_NOINTERFACE;
        NotifyBeginObserver(result, false, false, FALSE, kInvalidUiElementId);
        return result;
      }
      showing_ = true;
      OnPbShown(true);
      if (resume_secure_toast) own_window_.ResumeSecureToast();
      NotifyBeginObserver(S_OK, false, false, FALSE, kInvalidUiElementId);
      return S_OK;
    }

    BOOL pb_show = TRUE;
    DWORD ui_element_id = kInvalidUiElementId;
    hr = ui_element_mgr_->BeginUIElement(static_cast<ITfUIElement*>(ui_element_.get()), &pb_show,
                                         &ui_element_id);
    if (FAILED(hr)) {
      NotifyBeginObserver(hr, true, false, FALSE, kInvalidUiElementId);
      ReleaseUiElement();
      items_.clear();
      notice_.clear();
      selected_idx_ = -1;
      return hr;
    }

    ui_element_id_ = ui_element_id;
    showing_ = true;
    OnPbShown(pb_show != FALSE);
    if (resume_secure_toast && tip_draws_) own_window_.ResumeSecureToast();
    if (!tip_draws_) {
      ui_element_->Update(CandidateSurfaces(items_), selected_idx_, kAllInitialCandidateFlags);
      hr = ui_element_mgr_->UpdateUIElement(ui_element_id_);
      if (FAILED(hr)) {
        const HRESULT cleanup_hr = EndUI();
        UNREFERENCED_PARAMETER(cleanup_hr);
        NotifyBeginObserver(hr, true, true, pb_show, ui_element_id);
        return hr;
      }
    }
    NotifyBeginObserver(S_OK, true, true, pb_show, ui_element_id);
    return S_OK;
  } catch (const std::bad_alloc&) {
    const bool mgr_available = ui_element_mgr_ != nullptr;
    rollback_exception();
    NotifyBeginObserver(E_OUTOFMEMORY, mgr_available, false, FALSE, kInvalidUiElementId);
    return E_OUTOFMEMORY;
  } catch (...) {
    const bool mgr_available = ui_element_mgr_ != nullptr;
    rollback_exception();
    NotifyBeginObserver(E_FAIL, mgr_available, false, FALSE, kInvalidUiElementId);
    return E_FAIL;
  }
}

HRESULT CandidateUiCoordinator::UpdateUI(const std::vector<CandidateViewItem>& items,
                                         int selected_idx) {
  AZOOKEY_ASSERT_CANDIDATE_UI_THREAD();
  if (items.empty()) return EndUI();
  if (!showing_ || !ui_element_) return S_OK;

  items_ = items;
  selected_idx_ = ClampSelection(selected_idx);
  ui_element_->Update(CandidateSurfaces(items_), selected_idx_,
                      TF_CLUIE_COUNT | TF_CLUIE_STRING | TF_CLUIE_SELECTION);
  if (tip_draws_) {
    own_window_.Show(last_pt_, items_, selected_idx_, notice_);
    return S_OK;
  }
  if (ui_element_mgr_ && ui_element_id_ != kInvalidUiElementId) {
    return ui_element_mgr_->UpdateUIElement(ui_element_id_);
  }
  return S_OK;
}

HRESULT CandidateUiCoordinator::EndUI() {
  AZOOKEY_ASSERT_CANDIDATE_UI_THREAD();
  HRESULT result = S_OK;
  own_window_.Hide();
  if (showing_ && ui_element_mgr_ && ui_element_id_ != kInvalidUiElementId) {
    result = ui_element_mgr_->EndUIElement(ui_element_id_);
  }
  ui_element_id_ = kInvalidUiElementId;
  showing_ = false;
  tip_draws_ = true;
  items_.clear();
  notice_.clear();
  selected_idx_ = -1;
  ReleaseUiElement();
  return result;
}

HRESULT CandidateUiCoordinator::MoveSelection(int delta) {
  AZOOKEY_ASSERT_CANDIDATE_UI_THREAD();
  if (items_.empty()) return S_OK;
  const int count = static_cast<int>(items_.size());
  selected_idx_ = internal::WrapCandidateSelectionIndex(selected_idx_, delta, count);
  if (ui_element_) ui_element_->SetSelection(selected_idx_, TF_CLUIE_SELECTION);
  if (tip_draws_) {
    own_window_.SetSelected(selected_idx_);
    return S_OK;
  }
  if (ui_element_mgr_ && ui_element_id_ != kInvalidUiElementId) {
    return ui_element_mgr_->UpdateUIElement(ui_element_id_);
  }
  return S_OK;
}

HRESULT CandidateUiCoordinator::EnsureUiElementMgr(ITfThreadMgr* thread_mgr) {
  if (ui_element_mgr_) return S_OK;
  if (!thread_mgr) return E_INVALIDARG;
  HRESULT hr = thread_mgr->QueryInterface(IID_ITfUIElementMgr, ui_element_mgr_.put_void());
  if (FAILED(hr)) {
    ui_element_mgr_.reset();
    return hr;
  }
  return ui_element_mgr_ ? S_OK : E_NOINTERFACE;
}

void CandidateUiCoordinator::OnPbShown(bool tip_draws) {
  tip_draws_ = tip_draws;
  if (!ui_element_) return;
  ui_element_->SetShown(tip_draws_);
  if (tip_draws_) {
    own_window_.Show(last_pt_, items_, selected_idx_, notice_);
    ShowPendingSecureToast();
    ShowPendingHealthBanner();
  } else {
    own_window_.Hide();
  }
}

void CandidateUiCoordinator::ShowPendingHealthBanner() {
  if (!health_banner_pending_ || !own_window_.IsVisible()) return;
  own_window_.ShowHealthBanner(health_state_);
  health_banner_pending_ = false;
  if (health_state_ == CandidateHealthState::SafeMode) {
    notified_safe_mode_generations_.push_back(host_generation_id_);
  }
}

void CandidateUiCoordinator::ShowPendingSecureToast() {
  if (!secure_toast_pending_ || !own_window_.IsVisible()) return;
  own_window_.ShowSecureToast();
  secure_toast_pending_ = false;
}

void CandidateUiCoordinator::OnElementShow(bool show) {
  AZOOKEY_ASSERT_CANDIDATE_UI_THREAD();
  if (!show) {
    tip_draws_ = false;
    own_window_.Hide();
    if (showing_ && ui_element_ && ui_element_mgr_ && ui_element_id_ != kInvalidUiElementId) {
      ui_element_->Update(CandidateSurfaces(items_), selected_idx_, kAllInitialCandidateFlags);
      const HRESULT update_hr = ui_element_mgr_->UpdateUIElement(ui_element_id_);
      UNREFERENCED_PARAMETER(update_hr);
    }
    return;
  }
  tip_draws_ = true;
  if (showing_ && !items_.empty()) {
    own_window_.Show(last_pt_, items_, selected_idx_, notice_);
    ShowPendingSecureToast();
    ShowPendingHealthBanner();
  }
}

void CandidateUiCoordinator::ReleaseUiElement() {
  if (ui_element_) {
    ui_element_->SetShowCallback({});
    ui_element_.reset();
  }
}

void CandidateUiCoordinator::ReleaseUiElementMgr() { ui_element_mgr_.reset(); }

void CandidateUiCoordinator::NotifyBeginObserver(HRESULT result, bool ui_element_mgr_available,
                                                 bool pb_show_available, BOOL pb_show,
                                                 DWORD ui_element_id) const noexcept {
  if (!begin_observer_) return;
  CandidateUiBeginObservation observation;
  observation.result = result;
  observation.ui_element_id = ui_element_id;
  observation.ui_less = ui_less_mode_;
  observation.ui_element_mgr_available = ui_element_mgr_available;
  observation.pb_show_available = pb_show_available;
  observation.pb_show = pb_show_available && pb_show != FALSE;
  observation.tip_draws = SUCCEEDED(result) && tip_draws_;
  begin_observer_(observation, begin_observer_context_);
}

int CandidateUiCoordinator::ClampSelection(int selected_idx) const {
  if (items_.empty()) return -1;
  return std::clamp(selected_idx, 0, static_cast<int>(items_.size()) - 1);
}

}  // namespace azookey::tsf

#undef AZOOKEY_ASSERT_CANDIDATE_UI_THREAD
#undef AZOOKEY_ASSERT_BOUND_CANDIDATE_UI_THREAD
