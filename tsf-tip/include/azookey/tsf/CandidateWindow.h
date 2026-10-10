#pragma once

#include <Windows.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "azookey/tsf/ThemeColors.h"

struct ID2D1DeviceContext;

namespace azookey::tsf {

struct CandidateViewItem {
  std::wstring surface;
  std::wstring description;
};

enum class CandidateHealthState { Healthy, DegradedSimple, DegradedModel, SafeMode };

// Popup window that displays the IME candidate list.
// Must be created and used on the same thread (no internal locking).
class CandidateWindow {
 public:
  CandidateWindow();
  ~CandidateWindow();

  CandidateWindow(const CandidateWindow&) = delete;
  CandidateWindow& operator=(const CandidateWindow&) = delete;

  // Create the underlying HWND. Call once after the TIP is activated.
  bool Create();
  void Destroy();

  // Show at screen point 'pt' (bottom-left of the caret rect) with given items.
  // selected_idx is clamped to [0, items.size()).
  void Show(POINT pt, const std::vector<CandidateViewItem>& items, int selected_idx,
            std::wstring notice = {});
  void Hide();
  bool IsVisible() const;

  // Stage and HRESULT of the most recent drawing failure (native-ui-spec §4.1);
  // empty and S_OK after a committed frame. Never drawn content.
  const char* failure_stage() const { return failure_stage_; }
  HRESULT failure_hr() const { return failure_hr_; }
  void ShowHealthBanner(CandidateHealthState state);
  void HideHealthBanner();
  // Brings back a banner that a re-show of the window hid, for whatever is left
  // of its five seconds. No-op once that time has passed or the state is Healthy.
  void ResumeHealthBanner();
  bool IsHealthBannerVisible() const { return health_banner_visible_; }
  void SetRetryInFlight(bool in_flight);
  // M46 secure indicator: a small lock at the window's right edge while secure,
  // and a one-line toast that hides itself after five seconds.
  void SetSecureIndicator(bool visible);
  void ShowSecureToast();
  void HideSecureToast();
  // Brings back a toast that a re-show of the window hid, for whatever is left
  // of its five seconds. No-op once that time has passed.
  void ResumeSecureToast();
  bool IsSecureToastVisible() const { return secure_toast_visible_; }
  using OnRetryFn = std::function<void()>;
  void SetOnRetry(OnRetryFn fn) { on_retry_ = std::move(fn); }
  // Screen rect for the window: below the anchor, flipped above the caret when it
  // would overflow the bottom of the work area, and kept inside the work area.
  // An empty work area leaves the window at the anchor.
  static RECT ComputePlacement(POINT anchor, RECT work_area, int width, int height, int caret_gap);
  // Screen rect for the details popup: below the candidate window, or above it
  // when there is no room, kept inside the work area.
  static RECT ComputeDetailsPlacement(RECT candidate, RECT work_area, int width, int height);

  // What a left click at a client point lands on.
  enum class HitTarget {
    None,
    Candidate,
    SecureIndicator,
    HealthBanner,
    HealthDetailsButton,
    HealthRetryButton,
  };
  struct Hit {
    HitTarget target;
    int index;  // Candidate row; -1 for every other target.
  };
  // The click regions of one shown window, in client pixels.
  struct HitLayout {
    int item_height;
    int item_count;
    int client_width;
    int secure_indicator_width;  // 0 when the lock is hidden.
    bool health_banner_visible;
    int health_banner_top;
    RECT details_button;
    bool has_retry_button;
    RECT retry_button;
  };
  static Hit HitTest(const HitLayout& layout, POINT point);

  // Move selection by delta (+1 = down, -1 = up). Wraps around.
  void MoveSelection(int delta);
  void SetSelected(int idx);
  int GetSelected() const { return selected_idx_; }
  int GetCount() const { return static_cast<int>(items_.size()); }

  // Invoked when the user left-clicks a candidate row.
  using OnClickFn = std::function<void(int idx)>;
  void SetOnClick(OnClickFn fn) { on_click_ = std::move(fn); }

  // Posted from non-UI threads when cached candidates become available.
  using OnCandidatesReadyFn = void (*)(void* context);
  void SetOnCandidatesReady(OnCandidatesReadyFn fn, void* context) {
    on_candidates_ready_ = fn;
    on_candidates_ready_context_ = context;
  }
  void PostCandidatesReady();
  bool ScheduleCandidatesReady(UINT delay_ms);
  void CancelScheduledCandidatesReady();

#ifdef AZOOKEY_TSF_TESTING
  struct LayoutMetricsForTest {
    int item_height;
    int horizontal_padding;
    int max_width;
    int caret_gap;
    int min_text_width;
    int extra_width;
  };

  struct ColumnLayoutForTest {
    int surface_width;
    int column_gap;
    int content_width;
  };

  static LayoutMetricsForTest ComputeLayoutMetricsForTest(UINT dpi);
  static ColumnLayoutForTest ComputeColumnLayoutForTest(int max_surface_width,
                                                        int max_description_width, UINT dpi);
  LayoutMetricsForTest current_metrics_for_test() const {
    return {metrics_.item_height, metrics_.horizontal_padding, metrics_.max_width,
            metrics_.caret_gap,   metrics_.min_text_width,     metrics_.extra_width};
  }
  const std::vector<CandidateViewItem>& items_for_test() const { return items_; }
  HWND hwnd_for_test() const { return hwnd_; }
  const ThemeColors& theme_for_test() const { return theme_; }
  const std::wstring& notice_for_test() const { return notice_; }
  bool secure_indicator_visible_for_test() const { return secure_indicator_visible_; }
  bool secure_toast_visible_for_test() const { return secure_toast_visible_; }
  bool health_banner_visible_for_test() const { return health_banner_visible_; }
  int footer_top_for_test() const { return FooterTop(); }
  int health_banner_top_for_test() const { return HealthBannerTop(); }
  // Hit test against the window's current client width.
  Hit HitTestForTest(POINT point) const;
  // Pretends every monitor has this DPI; 0 restores the real lookup.
  static void SetMonitorDpiForTest(UINT dpi);
  // Draws the current content offscreen at the client size with grayscale text
  // and returns the pixels row by row as RGB values.
  bool RenderPixelsForTest(std::vector<COLORREF>* pixels, int* width, int* height) const;
  enum class RenderFailureForTest { None, Initialize, Draw };
  // Makes RenderingEngine initialization, or every frame, fail until reset.
  static void SetRenderFailureForTest(RenderFailureForTest failure);
  int render_init_failures_for_test() const { return render_init_failures_; }
  // Draws through the production path (RenderingEngine and the DComp surface)
  // at the client size; true when the frame was committed.
  bool RenderForTest();
#endif

 private:
  struct LayoutMetrics {
    int item_height;
    int horizontal_padding;
    int max_width;
    int caret_gap;
    int min_text_width;
    int extra_width;
  };

  struct ColumnLayout {
    int surface_width;
    int column_gap;
    int content_width;
  };

  static constexpr UINT kDefaultDpi = USER_DEFAULT_SCREEN_DPI;
  static constexpr int kBaseItemHeight = 24;
  static constexpr int kBaseHorzPad = 8;
  static constexpr int kBaseMaxWidth = 400;
  static constexpr int kBaseCaretGap = 20;
  static constexpr int kBaseMinTextWidth = 60;
  static constexpr int kBaseExtraWidth = 4;
  static constexpr int kBaseMaxSurfaceWidth = 220;
  static constexpr int kBaseColumnGap = 12;
  static constexpr UINT kCandidatesReadyMessage = WM_APP + 0x4b1;
  static constexpr UINT_PTR kCandidatesReadyTimer = 0x4b2;
  static constexpr UINT_PTR kHealthBannerTimer = 0x4b3;
  static constexpr UINT kHealthBannerDurationMs = 5000;
  static constexpr UINT_PTR kSecureToastTimer = 0x4b4;
  static constexpr UINT kSecureToastDurationMs = 5000;
  static constexpr int kBaseSecureIndicatorWidth = 24;

  HWND hwnd_{nullptr};
  HWND details_hwnd_{nullptr};
  UINT dpi_{kDefaultDpi};
  struct TextStyle;
  struct RenderState;
  std::unique_ptr<TextStyle> text_style_;
  std::unique_ptr<RenderState> render_;
  std::unique_ptr<RenderState> details_render_;
  const char* failure_stage_{""};
  HRESULT failure_hr_{S_OK};
  bool showing_{false};  // Show is moving and sizing the window.
  // The last frame failed, so the window is hidden while it should be shown.
  bool render_failed_{false};
  int render_init_failures_{0};
  LayoutMetrics metrics_{kBaseItemHeight, kBaseHorzPad,      kBaseMaxWidth,
                         kBaseCaretGap,   kBaseMinTextWidth, kBaseExtraWidth};
  std::vector<CandidateViewItem> items_;
  std::wstring notice_;
  int surface_column_width_{0};
  int selected_idx_{0};
  OnClickFn on_click_;
  OnRetryFn on_retry_;
  ThemeColors theme_{kLightTheme};
  CandidateHealthState health_state_{CandidateHealthState::Healthy};
  bool health_banner_visible_{false};
  ULONGLONG health_banner_until_{0};  // GetTickCount64 deadline of the last banner.
  bool retry_in_flight_{false};
  bool secure_indicator_visible_{false};
  bool secure_toast_visible_{false};
  ULONGLONG secure_toast_until_{0};  // GetTickCount64 deadline of the last toast.
  POINT last_anchor_{0, 0};
  OnCandidatesReadyFn on_candidates_ready_{nullptr};
  void* on_candidates_ready_context_{nullptr};

  static int ScaleForDpi(int value, UINT dpi);
  static LayoutMetrics ComputeLayoutMetrics(UINT dpi);
  static ColumnLayout ComputeColumnLayout(int max_surface_width, int max_description_width,
                                          UINT dpi);
  static UINT DpiForMonitor(HMONITOR monitor, HWND fallback_hwnd);
  static ATOM RegisterWindowClass();
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK DetailsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  // hwnd is the HWND from the WndProc delivery (authoritative; hwnd_ may be
  // null after WM_DESTROY, but trailing messages like WM_NCDESTROY still need
  // a valid handle for DefWindowProcW).
  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  void UpdateDpi(UINT dpi);
  void UpdateTheme();
  void Repaint();
  int MeasureText(const std::wstring& text) const;
  // Draws the window at this client size and commits it (native-ui-spec §4.1).
  bool Render(int width, int height);
  using DrawFn = void (CandidateWindow::*)(ID2D1DeviceContext*, int, int) const;
  // init_failures counts failed Initialize calls in a row; at the limit the
  // device stack is not created again.
  bool RenderTo(std::unique_ptr<RenderState>& state, int& init_failures, HWND hwnd, int width,
                int height, DrawFn draw) noexcept;
  static std::unique_ptr<TextStyle> CreateTextStyle(UINT dpi) noexcept;
  void DrawContent(ID2D1DeviceContext* context, int width, int height) const;
  void RenderDetails();
  void DrawDetails(ID2D1DeviceContext* context, int width, int height) const;
  bool Fail(const char* stage, HRESULT hr);
  void ResizeAtLastAnchor();
  void ShowDetails();
  void HideDetails();
  int HealthBannerHeight() const;
  // Candidate rows and the notice row end here; the secure toast row, then the
  // health banner, stack below it.
  int FooterTop() const;
  int HealthBannerTop() const;
  int SecureIndicatorWidth() const;
  RECT HealthDetailsButtonRect(int width) const;
  RECT HealthRetryButtonRect(int width) const;
  HitLayout CurrentHitLayout(int client_width) const;
};

}  // namespace azookey::tsf
