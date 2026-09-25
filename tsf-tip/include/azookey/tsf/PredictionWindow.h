#pragma once

#include <Windows.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

  using OnClickFn = std::function<void(int index)>;
  void SetOnClick(OnClickFn callback) { on_click_ = std::move(callback); }

  static constexpr std::size_t VisibleCount(std::size_t count) { return count < 5 ? count : 5; }
  static RECT ComputePlacement(RECT caret, RECT work_area, int width, int height);

 private:
  struct RenderState;

  static ATOM RegisterWindowClass();
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  bool InitializeRendering();
  bool ResizeSurface(int width, int height);
  bool Draw();
  int MeasureWidth() const;
  void UpdateDpi(UINT dpi);

  HWND hwnd_{nullptr};
  DWORD ui_thread_id_{0};
  UINT dpi_{USER_DEFAULT_SCREEN_DPI};
  int width_{0};
  int height_{0};
  int row_height_{28};
  int padding_{8};
  std::vector<std::wstring> candidates_;
  OnClickFn on_click_;
  std::unique_ptr<RenderState> render_;
};

}  // namespace azookey::tsf
