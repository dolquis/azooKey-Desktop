#pragma once

#include <Windows.h>
#include <msctf.h>
#include <wil/com.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "azookey/tsf/CandidateListUIElement.h"
#include "azookey/tsf/CandidateWindow.h"
#ifdef _DEBUG
#include "azookey/tsf/DebugThreadAffinity.h"
#endif

namespace azookey::tsf {

struct CandidateUiBeginObservation {
  HRESULT result{E_FAIL};
  DWORD ui_element_id{0xFFFFFFFF};
  bool ui_less{false};
  bool ui_element_mgr_available{false};
  bool pb_show_available{false};
  bool pb_show{false};
  bool tip_draws{false};
};

using CandidateUiBeginObserver = void (*)(const CandidateUiBeginObservation& observation,
                                          void* context) noexcept;

class CandidateUiCoordinator {
 public:
  // An app's SetSelection (index below the list's count), Finalize or Abort
  // (tsf-deep-integration-spec §2.3). The TIP decides what the list does.
  using OnBehaviorFn = std::function<HRESULT(CandidateListBehavior behavior, int index)>;

  CandidateUiCoordinator() = default;
  ~CandidateUiCoordinator();

  CandidateUiCoordinator(const CandidateUiCoordinator&) = delete;
  CandidateUiCoordinator& operator=(const CandidateUiCoordinator&) = delete;

  bool Create();
  void Destroy();

  void SetUiLessMode(bool ui_less);
  void SetOnClick(CandidateWindow::OnClickFn fn);
  // Reaches the TIP only for the list on show, and not while an app's sink
  // runs inside Begin/Update/EndUIElement or another such call is handled:
  // those calls get E_FAIL. `owner`, the object that holds this coordinator,
  // is not owned; each call holds a reference to it until the call has ended,
  // so a TIP that drops its last reference inside `fn` outlives this frame.
  void SetOnBehavior(OnBehaviorFn fn, IUnknown* owner);
  void SetOnRetry(CandidateWindow::OnRetryFn fn);
  // Called on the TSF UI thread after the IPC worker publishes a health snapshot.
  void SetHealthState(CandidateHealthState state, const std::string& host_generation_id);
  void SetRetryInFlight(bool in_flight);
  // Called on the TSF UI thread with every privacy resolution. Entering secure
  // queues one toast until the TIP next draws the candidate window; the lock
  // indicator follows the state while show_indicator holds.
  void SetSecureState(bool secure, bool show_indicator);
  void SetOnCandidatesReady(CandidateWindow::OnCandidatesReadyFn fn, void* context);
  void SetBeginObserver(CandidateUiBeginObserver observer, void* context);
  // False when no message could be posted (no window, or a full queue).
  bool PostCandidatesReady();
  bool ScheduleCandidatesReady(UINT delay_ms);
  void CancelScheduledCandidatesReady();

  // `anchor` is the rect the window opens at (CandidateWindow::Show): the
  // composition's text extent or a caret rect, the same as ShowPredictions.
  HRESULT BeginUI(ITfThreadMgr* thread_mgr, RECT anchor,
                  const std::vector<CandidateViewItem>& items, int selected_idx,
                  std::wstring notice = {});
  // A caret point with no height (CandidateWindow::Show).
  HRESULT BeginUI(ITfThreadMgr* thread_mgr, POINT pt, const std::vector<CandidateViewItem>& items,
                  int selected_idx, std::wstring notice = {}) {
    return BeginUI(thread_mgr, RECT{pt.x, pt.y, pt.x, pt.y}, items, selected_idx,
                   std::move(notice));
  }
  HRESULT UpdateUI(const std::vector<CandidateViewItem>& items, int selected_idx);
  HRESULT EndUI();

  bool IsShowing() const { return showing_; }

  // Prediction mode of the same window (native-ui-spec §4.2). Predictions never
  // reach the ITfCandidateListUIElement (§5.2), so the TIP draws them even where
  // the app draws the candidate list. Ignored while the candidate list is shown.
  // False when the window could not be created.
  bool ShowPredictions(RECT anchor, const std::vector<std::wstring>& predictions);
  // Hides the window only while it is in prediction mode.
  void HidePredictions();
  // True only while prediction rows are actually on screen.
  bool IsShowingPredictions() const { return !showing_ && own_window_.IsShowingPredictions(); }
  void SetOnPredictionClick(CandidateWindow::OnPredictionClickFn fn);
  // Stage and HRESULT of the window's last drawing failure; never drawn content.
  const char* window_failure_stage() const { return own_window_.failure_stage(); }
  HRESULT window_failure_hr() const { return own_window_.failure_hr(); }
  HRESULT MoveSelection(int delta);
  // Highlights row `selected_idx` in the window and the UI element alike.
  HRESULT SetSelection(int selected_idx);
  int GetSelected() const { return selected_idx_; }
  int GetCount() const { return static_cast<int>(items_.size()); }

  // Candidate pages (legacy-parity-spec §1.4「候補のページ」). The split digits
  // and PageUp / PageDown use: the one an app set with SetPageIndex while the
  // app draws the list, otherwise empty for nine per page, as the window draws
  // (tsf-deep-integration-spec §2.3). The TIP attaches it to the core's events.
  const std::vector<uint32_t>& PageSplit() const;
  // The candidate a digit 1-9 picks on the current page; -1 when it has none.
  int DigitTarget(int digit) const;
  // Moves the selection to the same position on the next (forward) or previous
  // page, or that page's last candidate. S_FALSE without a move on one page.
  HRESULT MovePage(bool forward);

#ifdef AZOOKEY_TSF_TESTING
  const std::vector<CandidateViewItem>& items_for_test() const { return items_; }
  bool health_banner_pending_for_test() const { return health_banner_pending_; }
  bool secure_toast_pending_for_test() const { return secure_toast_pending_; }
  const CandidateWindow& own_window_for_test() const { return own_window_; }
  // The list's UI element, as an app reaches it; null while none is shown.
  CandidateListUIElement* ui_element_for_test() const { return ui_element_.get(); }
#endif

 private:
  static constexpr DWORD kInvalidUiElementId = 0xFFFFFFFF;

  HRESULT EnsureUiElementMgr(ITfThreadMgr* thread_mgr);
  // UpdateUIElement with an app's Behavior calls held off (SetOnBehavior).
  HRESULT NotifyUiElementUpdated();
  void OnPbShown(bool tip_draws);
  void OnElementShow(bool show);
  static HRESULT OnElementBehavior(CandidateListBehavior behavior, UINT index,
                                   void* context) noexcept;
  void ShowPendingHealthBanner();
  void ShowPendingSecureToast();
  // The window is on screen with candidate rows, not predictions.
  bool CandidateRowsVisible() const;
  void OnWindowRenderResult(bool drawn) noexcept;
  void ReleaseUiElement();
  void ReleaseUiElementMgr();
  void NotifyBeginObserver(HRESULT result, bool ui_element_mgr_available, bool pb_show_available,
                           BOOL pb_show, DWORD ui_element_id) const noexcept;
  int ClampSelection(int selected_idx) const;

#ifdef _DEBUG
  DebugThreadAffinity ui_thread_affinity_;
#endif
  CandidateWindow own_window_;
  wil::com_ptr_nothrow<CandidateListUIElement> ui_element_;
  wil::com_ptr_nothrow<ITfUIElementMgr> ui_element_mgr_;
  DWORD ui_element_id_{kInvalidUiElementId};
  std::vector<CandidateViewItem> items_;
  std::wstring notice_;
  RECT last_anchor_{0, 0, 0, 0};
  int selected_idx_{-1};
  bool ui_less_mode_{false};
  bool tip_draws_{true};
  bool showing_{false};
  bool window_created_{false};
  CandidateHealthState health_state_{CandidateHealthState::Healthy};
  bool health_banner_pending_{false};
  std::string host_generation_id_;
  std::vector<std::string> notified_safe_mode_generations_;
  // A new TIP instance starts outside secure, so its first secure resolution
  // counts as entering it.
  bool secure_{false};
  bool show_secure_indicator_{true};
  bool secure_toast_pending_{false};
  CandidateUiBeginObserver begin_observer_{nullptr};
  void* begin_observer_context_{nullptr};
  OnBehaviorFn on_behavior_;
  IUnknown* behavior_owner_{nullptr};  // Not owned (SetOnBehavior).
  // Inside Begin/Update/EndUIElement or the handling of a Behavior call.
  bool calling_out_{false};
  std::vector<std::string> logged_render_failure_stages_;
  bool handling_render_result_{false};
};

}  // namespace azookey::tsf
