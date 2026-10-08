#pragma once

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <string>

#include "azookey/core/PrivacyPolicy.h"
#include "azookey/tsf/DebugLogBuffer.h"

namespace azookey::tsf {

// Semi-transparent 600x400 popup that paints DebugLogBuffer with GDI
// (docs/legacy-parity-spec.md §8). Every recorded line is also mirrored to
// OutputDebugStringW after the M41 body gate is applied. Create, Destroy,
// Show, Hide and Toggle must run on the creating UI thread; Record* and
// Invalidate may be called from any thread.
class DebugWindow {
 public:
  DebugWindow() = default;
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

 private:
  static ATOM RegisterWindowClass();
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  void Paint(HWND hwnd);
  bool BodyAllowedForPaint() const;
  void Mirror(const std::string& line);

  DebugLogBuffer buffer_;
  std::atomic<HWND> hwnd_{nullptr};
  DWORD ui_thread_id_{0};
  mutable std::mutex policy_mutex_;
  core::PrivacyPolicy paint_policy_{};
};

}  // namespace azookey::tsf
