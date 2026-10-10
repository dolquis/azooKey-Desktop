#include "azookey/tsf/RenderingEngine.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace azookey::tsf {

using Microsoft::WRL::ComPtr;

struct RenderingEngine::State {
  ComPtr<ID3D11Device> d3d_device;
  ComPtr<ID2D1Factory1> d2d_factory;
  ComPtr<ID2D1Device> d2d_device;
  ComPtr<IDCompositionDevice2> composition_device;
  ComPtr<IDCompositionDesktopDevice> desktop_device;
  ComPtr<IDCompositionTarget> target;
  ComPtr<IDCompositionVisual2> visual;
  ComPtr<IDCompositionSurface> surface;
  ComPtr<IDWriteFactory> write_factory;
  ComPtr<ID2D1DeviceContext> drawing;
  SurfaceAlpha alpha{SurfaceAlpha::Opaque};
  int surface_width{0};
  int surface_height{0};
};

HRESULT CreateMessageTextFormat(IDWriteFactory* factory, UINT dpi, IDWriteTextFormat** format) {
  if (!factory || !format) return E_POINTER;
  dpi = dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof(metrics);
  const wchar_t* family = L"Yu Gothic UI";
  FLOAT size = 9.0f * static_cast<FLOAT>(dpi) / 72.0f;
  DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_REGULAR;
  if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
    const LOGFONTW& font = metrics.lfMessageFont;
    if (font.lfFaceName[0]) family = font.lfFaceName;
    // A negative height is the em size; a positive one is the cell height, which
    // GDI also maps to a slightly smaller em size. Both are used as the em size.
    if (font.lfHeight != 0)
      size = static_cast<FLOAT>(font.lfHeight < 0 ? -font.lfHeight : font.lfHeight);
    if (font.lfWeight > 0) weight = static_cast<DWRITE_FONT_WEIGHT>(font.lfWeight);
  }
  return factory->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                   DWRITE_FONT_STRETCH_NORMAL, size, L"ja-JP", format);
}

RenderingEngine::RenderingEngine() = default;
RenderingEngine::~RenderingEngine() = default;

bool RenderingEngine::Fail(const char* stage, HRESULT hr) {
  failure_stage_ = stage;
  failure_hr_ = hr;
  return false;
}

bool RenderingEngine::Initialize(HWND hwnd, SurfaceAlpha alpha) {
  failure_stage_ = "";
  failure_hr_ = S_OK;
  auto state = std::make_unique<State>();
  state->alpha = alpha;
  constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                 D3D11_SDK_VERSION, &state->d3d_device, nullptr, nullptr);
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                           D3D11_SDK_VERSION, &state->d3d_device, nullptr, nullptr);
  }
  if (FAILED(hr)) return Fail("d3d11_device", hr);

  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(hr = state->d3d_device.As(&dxgi_device))) return Fail("dxgi_device", hr);
  if (FAILED(hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                    nullptr,
                                    reinterpret_cast<void**>(state->d2d_factory.GetAddressOf()))))
    return Fail("d2d_factory", hr);
  if (FAILED(hr = state->d2d_factory->CreateDevice(dxgi_device.Get(), &state->d2d_device)))
    return Fail("d2d_device", hr);

  // A Direct2D device is required here so BeginDraw can return a device context.
  // DCompositionCreateDevice2 accepts only IDCompositionDevice or
  // IDCompositionDesktopDevice; asking for IDCompositionDevice2 fails with
  // E_NOINTERFACE, so query it from the desktop device instead.
  if (FAILED(hr = DCompositionCreateDevice2(
                 state->d2d_device.Get(), __uuidof(IDCompositionDesktopDevice),
                 reinterpret_cast<void**>(state->desktop_device.GetAddressOf()))))
    return Fail("dcomp_device", hr);
  if (FAILED(hr = state->desktop_device.As(&state->composition_device)))
    return Fail("dcomp_device2", hr);
  if (FAILED(hr = state->desktop_device->CreateTargetForHwnd(hwnd, TRUE, &state->target)))
    return Fail("dcomp_target", hr);
  if (FAILED(hr = state->composition_device->CreateVisual(&state->visual)) ||
      FAILED(hr = state->target->SetRoot(state->visual.Get())))
    return Fail("dcomp_visual", hr);

  if (FAILED(hr = DWriteCreateFactory(
                 DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                 reinterpret_cast<IUnknown**>(state->write_factory.GetAddressOf()))))
    return Fail("dwrite_factory", hr);

  state_ = std::move(state);
  return true;
}

void RenderingEngine::Reset() { state_.reset(); }

bool RenderingEngine::IsInitialized() const { return state_ != nullptr; }

IDWriteFactory* RenderingEngine::write_factory() const {
  return state_ ? state_->write_factory.Get() : nullptr;
}

bool RenderingEngine::ResizeSurface(int width, int height) {
  if (!state_) return Fail("no_render", E_FAIL);
  if (state_->surface && state_->surface_width == width && state_->surface_height == height)
    return true;

  ComPtr<IDCompositionSurface> surface;
  HRESULT hr = S_OK;
  if (FAILED(hr = state_->composition_device->CreateSurface(
                 width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                 state_->alpha == SurfaceAlpha::Opaque ? DXGI_ALPHA_MODE_IGNORE
                                                       : DXGI_ALPHA_MODE_PREMULTIPLIED,
                 &surface)) ||
      FAILED(hr = state_->visual->SetContent(surface.Get())))
    return Fail("surface", hr);
  state_->surface = std::move(surface);
  state_->surface_width = width;
  state_->surface_height = height;
  return true;
}

ID2D1DeviceContext* RenderingEngine::BeginDraw() {
  if (!state_ || !state_->surface) {
    Fail("no_render", E_FAIL);
    return nullptr;
  }
  POINT offset{};
  if (const HRESULT hr = state_->surface->BeginDraw(
          nullptr, __uuidof(ID2D1DeviceContext),
          reinterpret_cast<void**>(state_->drawing.ReleaseAndGetAddressOf()), &offset);
      FAILED(hr)) {
    Fail("begin_draw", hr);
    return nullptr;
  }
  state_->drawing->SetDpi(96.0f, 96.0f);
  state_->drawing->SetTransform(
      D2D1::Matrix3x2F::Translation(static_cast<FLOAT>(offset.x), static_cast<FLOAT>(offset.y)));
  return state_->drawing.Get();
}

bool RenderingEngine::EndDraw() {
  if (!state_ || !state_->surface) return Fail("no_render", E_FAIL);
  const HRESULT end_draw = state_->surface->EndDraw();
  state_->drawing.Reset();
  if (FAILED(end_draw)) return Fail("end_draw", end_draw);
  if (const HRESULT hr = state_->composition_device->Commit(); FAILED(hr))
    return Fail("commit", hr);
  return true;
}

}  // namespace azookey::tsf
