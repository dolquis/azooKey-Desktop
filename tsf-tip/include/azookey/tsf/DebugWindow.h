#pragma once

#include <Windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "azookey/core/PrivacyPolicy.h"
#include "azookey/tsf/DebugLogBuffer.h"
#include "azookey/tsf/ThemeColors.h"

namespace azookey::tsf {

// Semi-transparent 600x400 popup that draws DebugLogBuffer with RenderingEngine
// in the theme colors (docs/legacy-parity-spec.md §8, docs/native-ui-spec.md
// §4.3). Every recorded line is also mirrored to OutputDebugStringW after the
// M41 body gate is applied. Create, Destroy, Show, Hide and Toggle must run on
// the creating UI thread; Record* and Invalidate may be called from any thread.
class DebugWindow {
 public:
  DebugWindow();
  ~DebugWindow();

  DebugWindow(const DebugWindow&) = delete;
  DebugWindow& operator=(const DebugWindow&) = delete;

  bool Create();
  void Destroy();
  void Show();
  void Hide();
  void Toggle();
  bool IsVisible() const;
  void Invalidate();

  // Policy used for painting. The default policy is secure, so body text stays
  // hidden until the caller supplies a policy that allows detailed logging.
  void SetPrivacyPolicy(const core::PrivacyPolicy& policy);
  // `privacy` is the event-local policy of the logged request.
  void RecordIpc(DebugIpcLogEntry entry, const core::PrivacyPolicy& privacy) noexcept;
  void RecordTransition(DebugStateTransitionEntry entry) noexcept;

  const DebugLogBuffer& buffer() const { return buffer_; }

#ifdef AZOOKEY_TSF_TESTING
  HWND hwnd_for_test() const { return hwnd_.load(); }
  bool rendering_for_test() const { return render_ != nullptr; }
#endif

 private:
  static ATOM RegisterWindowClass();
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  void Paint(HWND hwnd);
  void Render(HWND hwnd);
  void UpdateTheme();
  bool BodyAllowedForPaint() const;
  void Mirror(const std::string& line);

  DebugLogBuffer buffer_;
  std::atomic<HWND> hwnd_{nullptr};
  DWORD ui_thread_id_{0};
  mutable std::mutex policy_mutex_;
  core::PrivacyPolicy paint_policy_{};
  // Created, drawn and reset on the UI thread. After a Destroy from another
  // thread it stays until the next Create or ~DebugWindow, which may run there.
  struct RenderState;
  std::unique_ptr<RenderState> render_;
  int render_init_failures_{0};  // Initialize stops retrying after a few failures.
  ThemeMode theme_mode_{ThemeMode::Light};
  ThemeColors theme_{kLightTheme};
};

}  // namespace azookey::tsf
