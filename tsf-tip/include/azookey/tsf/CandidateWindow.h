#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace azookey::tsf {

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
  void Show(POINT pt, const std::vector<std::wstring>& items, int selected_idx);
  void Hide();
  bool IsVisible() const;

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

#ifdef AZOOKEY_TSF_TESTING
  struct LayoutMetricsForTest {
    int item_height;
    int horizontal_padding;
    int max_width;
    int caret_gap;
    int min_text_width;
    int extra_width;
  };

  static LayoutMetricsForTest ComputeLayoutMetricsForTest(UINT dpi);
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

  static constexpr UINT kDefaultDpi = USER_DEFAULT_SCREEN_DPI;
  static constexpr int kBaseItemHeight = 24;
  static constexpr int kBaseHorzPad = 8;
  static constexpr int kBaseMaxWidth = 400;
  static constexpr int kBaseCaretGap = 20;
  static constexpr int kBaseMinTextWidth = 60;
  static constexpr int kBaseExtraWidth = 4;
  static constexpr UINT kCandidatesReadyMessage = WM_APP + 0x4b1;

  HWND hwnd_{nullptr};
  UINT dpi_{kDefaultDpi};
  HFONT font_{nullptr};
  LayoutMetrics metrics_{kBaseItemHeight, kBaseHorzPad,      kBaseMaxWidth,
                         kBaseCaretGap,   kBaseMinTextWidth, kBaseExtraWidth};
  std::vector<std::wstring> items_;
  int selected_idx_{0};
  OnClickFn on_click_;
  OnCandidatesReadyFn on_candidates_ready_{nullptr};
  void* on_candidates_ready_context_{nullptr};

  static int ScaleForDpi(int value, UINT dpi);
  static LayoutMetrics ComputeLayoutMetrics(UINT dpi);
  static UINT DpiForMonitor(HMONITOR monitor, HWND fallback_hwnd);
  static HFONT CreateMessageFont(UINT dpi);
  static ATOM RegisterWindowClass();
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  // hwnd is the HWND from the WndProc delivery (authoritative; hwnd_ may be
  // null after WM_DESTROY, but trailing messages like WM_NCDESTROY still need
  // a valid handle for DefWindowProcW).
  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  void UpdateDpi(UINT dpi);
  void Repaint() const;
};

}  // namespace azookey::tsf
