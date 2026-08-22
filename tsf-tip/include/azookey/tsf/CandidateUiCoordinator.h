#pragma once

#include <Windows.h>
#include <msctf.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "azookey/tsf/CandidateListUIElement.h"
#include "azookey/tsf/CandidateWindow.h"

namespace azookey::tsf {

class CandidateUiCoordinator {
 public:
  CandidateUiCoordinator() = default;
  ~CandidateUiCoordinator();

  CandidateUiCoordinator(const CandidateUiCoordinator&) = delete;
  CandidateUiCoordinator& operator=(const CandidateUiCoordinator&) = delete;

  bool Create();
  void Destroy();

  void SetUiLessMode(bool ui_less);
  void SetOnClick(CandidateWindow::OnClickFn fn);
  void SetOnCandidatesReady(CandidateWindow::OnCandidatesReadyFn fn, void* context);
  void PostCandidatesReady();

  HRESULT BeginUI(ITfThreadMgr* thread_mgr, POINT pt,
                  const std::vector<CandidateViewItem>& items,
                  int selected_idx);
  HRESULT UpdateUI(const std::vector<CandidateViewItem>& items, int selected_idx);
  HRESULT EndUI();

  bool IsShowing() const { return showing_; }
  HRESULT MoveSelection(int delta);
  int GetSelected() const { return selected_idx_; }
  int GetCount() const { return static_cast<int>(items_.size()); }

#ifdef AZOOKEY_TSF_TESTING
  const std::vector<CandidateViewItem>& items_for_test() const { return items_; }
#endif

 private:
  static constexpr DWORD kInvalidUiElementId = 0xFFFFFFFF;

  HRESULT EnsureUiElementMgr(ITfThreadMgr* thread_mgr);
  void OnPbShown(bool tip_draws);
  void OnElementShow(bool show);
  void ReleaseUiElement();
  void ReleaseUiElementMgr();
  int ClampSelection(int selected_idx) const;

  CandidateWindow own_window_;
  CandidateListUIElement* ui_element_{nullptr};
  ITfUIElementMgr* ui_element_mgr_{nullptr};
  DWORD ui_element_id_{kInvalidUiElementId};
  std::vector<CandidateViewItem> items_;
  POINT last_pt_{0, 0};
  int selected_idx_{-1};
  bool ui_less_mode_{false};
  bool tip_draws_{true};
  bool showing_{false};
};

}  // namespace azookey::tsf
