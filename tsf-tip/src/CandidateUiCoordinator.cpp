#include "azookey/tsf/CandidateUiCoordinator.h"

#include <algorithm>
#include <new>
#include <utility>

#include "CandidateSelection.h"

namespace azookey::tsf {

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

bool CandidateUiCoordinator::Create() { return own_window_.Create(); }

void CandidateUiCoordinator::Destroy() {
  EndUI();
  own_window_.Destroy();
  ReleaseUiElementMgr();
}

void CandidateUiCoordinator::SetUiLessMode(bool ui_less) { ui_less_mode_ = ui_less; }

void CandidateUiCoordinator::SetOnClick(CandidateWindow::OnClickFn fn) {
  own_window_.SetOnClick(std::move(fn));
}

void CandidateUiCoordinator::SetOnCandidatesReady(CandidateWindow::OnCandidatesReadyFn fn,
                                                  void* context) {
  own_window_.SetOnCandidatesReady(fn, context);
}

void CandidateUiCoordinator::PostCandidatesReady() { own_window_.PostCandidatesReady(); }

HRESULT CandidateUiCoordinator::BeginUI(ITfThreadMgr* thread_mgr, POINT pt,
                                        const std::vector<CandidateViewItem>& items,
                                        int selected_idx) {
  if (items.empty()) return EndUI();

  const HRESULT end_hr = EndUI();
  if (FAILED(end_hr)) return end_hr;

  items_ = items;
  selected_idx_ = ClampSelection(selected_idx);
  last_pt_ = pt;

  auto* element = new (std::nothrow) CandidateListUIElement(CandidateSurfaces(items_), selected_idx_);
  if (!element) return E_OUTOFMEMORY;
  ui_element_ = element;
  ui_element_->SetShowCallback([this](bool show) { OnElementShow(show); });

  HRESULT hr = EnsureUiElementMgr(thread_mgr);
  if (FAILED(hr) || !ui_element_mgr_) {
    if (ui_less_mode_) {
      ReleaseUiElement();
      items_.clear();
      selected_idx_ = -1;
      return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    showing_ = true;
    OnPbShown(true);
    return S_OK;
  }

  BOOL pb_show = TRUE;
  DWORD ui_element_id = kInvalidUiElementId;
  hr = ui_element_mgr_->BeginUIElement(static_cast<ITfUIElement*>(ui_element_), &pb_show,
                                       &ui_element_id);
  if (FAILED(hr)) {
    ReleaseUiElement();
    items_.clear();
    selected_idx_ = -1;
    return hr;
  }

  ui_element_id_ = ui_element_id;
  showing_ = true;
  OnPbShown(pb_show != FALSE);
  if (!tip_draws_) {
    ui_element_->Update(CandidateSurfaces(items_), selected_idx_, kAllInitialCandidateFlags);
    hr = ui_element_mgr_->UpdateUIElement(ui_element_id_);
    if (FAILED(hr)) {
      const HRESULT cleanup_hr = EndUI();
      UNREFERENCED_PARAMETER(cleanup_hr);
      return hr;
    }
  }
  return S_OK;
}

HRESULT CandidateUiCoordinator::UpdateUI(const std::vector<CandidateViewItem>& items,
                                         int selected_idx) {
  if (items.empty()) return EndUI();
  if (!showing_ || !ui_element_) return S_OK;

  items_ = items;
  selected_idx_ = ClampSelection(selected_idx);
  ui_element_->Update(CandidateSurfaces(items_), selected_idx_,
                      TF_CLUIE_COUNT | TF_CLUIE_STRING | TF_CLUIE_SELECTION);
  if (tip_draws_) {
    own_window_.Show(last_pt_, items_, selected_idx_);
    return S_OK;
  }
  if (ui_element_mgr_ && ui_element_id_ != kInvalidUiElementId) {
    return ui_element_mgr_->UpdateUIElement(ui_element_id_);
  }
  return S_OK;
}

HRESULT CandidateUiCoordinator::EndUI() {
  HRESULT result = S_OK;
  own_window_.Hide();
  if (showing_ && ui_element_mgr_ && ui_element_id_ != kInvalidUiElementId) {
    result = ui_element_mgr_->EndUIElement(ui_element_id_);
  }
  ui_element_id_ = kInvalidUiElementId;
  showing_ = false;
  tip_draws_ = true;
  items_.clear();
  selected_idx_ = -1;
  ReleaseUiElement();
  return result;
}

HRESULT CandidateUiCoordinator::MoveSelection(int delta) {
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
  HRESULT hr =
      thread_mgr->QueryInterface(IID_ITfUIElementMgr, reinterpret_cast<void**>(&ui_element_mgr_));
  if (FAILED(hr)) {
    ui_element_mgr_ = nullptr;
    return hr;
  }
  return ui_element_mgr_ ? S_OK : E_NOINTERFACE;
}

void CandidateUiCoordinator::OnPbShown(bool tip_draws) {
  tip_draws_ = tip_draws;
  if (!ui_element_) return;
  ui_element_->SetShown(tip_draws_);
  if (tip_draws_) {
    own_window_.Show(last_pt_, items_, selected_idx_);
  } else {
    own_window_.Hide();
  }
}

void CandidateUiCoordinator::OnElementShow(bool show) {
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
  if (showing_ && !items_.empty()) own_window_.Show(last_pt_, items_, selected_idx_);
}

void CandidateUiCoordinator::ReleaseUiElement() {
  if (ui_element_) {
    ui_element_->SetShowCallback({});
    ui_element_->Release();
    ui_element_ = nullptr;
  }
}

void CandidateUiCoordinator::ReleaseUiElementMgr() {
  if (ui_element_mgr_) {
    ui_element_mgr_->Release();
    ui_element_mgr_ = nullptr;
  }
}

int CandidateUiCoordinator::ClampSelection(int selected_idx) const {
  if (items_.empty()) return -1;
  return std::clamp(selected_idx, 0, static_cast<int>(items_.size()) - 1);
}

}  // namespace azookey::tsf
