#pragma once

#include <Windows.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "azookey/tsf/ThemeColors.h"

namespace azookey::tsf {

// Owns a separate, non-activating popup for predictions. All methods except
// ComputePlacement and VisibleCount must be called on the creating UI thread.
class PredictionWindow {
 public:
  PredictionWindow();
  ~PredictionWindow();

  PredictionWindow(const PredictionWindow&) = delete;
  PredictionWindow& operator=(const PredictionWindow&) = delete;

  bool Create();
  void Destroy();
  void Show(const std::vector<std::wstring>& candidates, RECT caret_rect_screen);
  void Hide();
  bool IsVisible() const;
  // Stage and HRESULT of the most recent Create or Show failure; never input text.
  const char* failure_stage() const { return failure_stage_; }
  HRESULT failure_hr() const { return failure_hr_; }

  using OnClickFn = std::function<void(int index)>;
  void SetOnClick(OnClickFn callback) { on_click_ = std::move(callback); }

  static constexpr std::size_t VisibleCount(std::size_t count) { return count < 5 ? count : 5; }
  static RECT ComputePlacement(RECT caret, RECT work_area, int width, int height);

#ifdef AZOOKEY_TSF_TESTING
  HWND hwnd_for_test() const { return hwnd_; }
  const ThemeColors& theme_for_test() const { return theme_; }
#endif

 private:
  struct RenderState;

  static ATOM RegisterWindowClass();
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  bool InitializeRendering();
  bool Fail(const char* stage, HRESULT hr);
  bool ResizeSurface(int width, int height);
  bool Draw();
  int MeasureWidth() const;
  void UpdateDpi(UINT dpi);
  void UpdateTheme();

  HWND hwnd_{nullptr};
  DWORD ui_thread_id_{0};
  UINT dpi_{USER_DEFAULT_SCREEN_DPI};
  int width_{0};
  int height_{0};
  int row_height_{28};
  int padding_{8};
  // Show already measures and places for the target monitor; a WM_DPICHANGED
  // raised by its own SetWindowPos must not resize the window again.
  bool showing_{false};
  RECT last_caret_rect_{0, 0, 0, 0};
  std::vector<std::wstring> candidates_;
  OnClickFn on_click_;
  ThemeMode theme_mode_{ThemeMode::Light};
  ThemeColors theme_{kLightTheme};
  std::unique_ptr<RenderState> render_;
  const char* failure_stage_{""};
  HRESULT failure_hr_{S_OK};
};

}  // namespace azookey::tsf
