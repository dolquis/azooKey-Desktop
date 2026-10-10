#pragma once

#include <Windows.h>

#include <memory>

struct ID2D1DeviceContext;
struct IDWriteFactory;
struct IDWriteTextFormat;

namespace azookey::tsf {

// How the surface's alpha channel is composed (native-ui-spec §2.1).
enum class SurfaceAlpha : unsigned char {
  // The alpha channel is ignored; the window is opaque and text keeps ClearType.
  Opaque,
  // Premultiplied alpha; the desktop shows through and text is grayscale.
  Premultiplied,
};

// Creates a text format from the system message font (SPI_GETNONCLIENTMETRICS)
// at the given DPI, in pixels for a 96 DPI device context. Falls back to
// 9 pt Yu Gothic UI when the metrics cannot be read.
HRESULT CreateMessageTextFormat(IDWriteFactory* factory, UINT dpi, IDWriteTextFormat** format);

// DirectComposition + Direct2D + DirectWrite stack for one popup HWND
// (native-ui-spec §2.2, §3). The window should use WS_EX_NOREDIRECTIONBITMAP.
// All methods run on the thread that owns the window.
class RenderingEngine {
 public:
  RenderingEngine();
  ~RenderingEngine();

  RenderingEngine(const RenderingEngine&) = delete;
  RenderingEngine& operator=(const RenderingEngine&) = delete;

  // Creates the devices and a composition target for the window.
  bool Initialize(HWND hwnd, SurfaceAlpha alpha);
  void Reset();
  bool IsInitialized() const;

  // Keeps a surface of exactly this size in physical pixels.
  bool ResizeSurface(int width, int height);

  // Starts drawing the whole surface in physical pixels (96 DPI device context,
  // origin at the surface's top-left). Returns null on failure; EndDraw must
  // follow a successful call.
  ID2D1DeviceContext* BeginDraw();
  // Ends drawing and commits the composition.
  bool EndDraw();

  IDWriteFactory* write_factory() const;

  // Stage and HRESULT of the most recent failure; never drawn content.
  const char* failure_stage() const { return failure_stage_; }
  HRESULT failure_hr() const { return failure_hr_; }

 private:
  struct State;
  bool Fail(const char* stage, HRESULT hr);

  std::unique_ptr<State> state_;
  const char* failure_stage_{""};
  HRESULT failure_hr_{S_OK};
};

}  // namespace azookey::tsf
