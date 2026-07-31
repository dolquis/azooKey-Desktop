#include "runner/ScreenshotCapture.h"

#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace azookey::compat_test {
namespace {

template <typename T>
void SafeRelease(T*& value) {
  if (value) {
    value->Release();
    value = nullptr;
  }
}

RECT IntersectWithImage(RECT rect, POINT origin, int width, int height) {
  rect.left = std::max<LONG>(rect.left - origin.x, 0);
  rect.top = std::max<LONG>(rect.top - origin.y, 0);
  rect.right = std::min<LONG>(rect.right - origin.x, width);
  rect.bottom = std::min<LONG>(rect.bottom - origin.y, height);
  return rect;
}

}  // namespace

void RenderGeometryOverlay(uint8_t* bgra, int width, int height, int stride, POINT origin,
                           const std::vector<RECT>& highlight_rects) {
  if (!bgra || width <= 0 || height <= 0 || stride < width * 4) return;
  for (int y = 0; y < height; ++y) {
    uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    std::memset(row, 0, static_cast<size_t>(width) * 4);
    for (int x = 0; x < width; ++x) row[static_cast<size_t>(x) * 4 + 3] = 0xff;
  }

  const auto draw_border = [&](RECT rect, uint8_t blue, uint8_t green, uint8_t red) {
    rect = IntersectWithImage(rect, origin, width, height);
    if (rect.left >= rect.right || rect.top >= rect.bottom) return;
    constexpr LONG kBorderWidth = 3;
    for (LONG y = rect.top; y < rect.bottom; ++y) {
      for (LONG x = rect.left; x < rect.right; ++x) {
        const bool border = x < rect.left + kBorderWidth || x >= rect.right - kBorderWidth ||
                            y < rect.top + kBorderWidth || y >= rect.bottom - kBorderWidth;
        if (!border) continue;
        uint8_t* pixel =
            bgra + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
        pixel[0] = blue;
        pixel[1] = green;
        pixel[2] = red;
        pixel[3] = 0xff;
      }
    }
  };
  for (size_t index = 0; index < highlight_rects.size(); ++index) {
    if (index == 0) {
      draw_border(highlight_rects[index], 0xff, 0x80, 0x00);
    } else {
      draw_border(highlight_rects[index], 0x00, 0x80, 0xff);
    }
  }
}

/*
 * The saved image is a geometry-only overlay. No desktop pixels are acquired;
 * only the supplied rectangles are drawn into a blank buffer.
 */
bool WriteGeometryOverlayPng(const std::filesystem::path& path,
                             const std::vector<RECT>& highlight_rects) {
  if (highlight_rects.empty()) return false;
  const RECT virtual_screen{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                            GetSystemMetrics(SM_XVIRTUALSCREEN) +
                                GetSystemMetrics(SM_CXVIRTUALSCREEN),
                            GetSystemMetrics(SM_YVIRTUALSCREEN) +
                                GetSystemMetrics(SM_CYVIRTUALSCREEN)};
  RECT capture = highlight_rects.front();
  for (const RECT rect : highlight_rects) {
    capture.left = std::min(capture.left, rect.left);
    capture.top = std::min(capture.top, rect.top);
    capture.right = std::max(capture.right, rect.right);
    capture.bottom = std::max(capture.bottom, rect.bottom);
  }
  constexpr LONG kContextMargin = 24;
  capture.left = std::max(virtual_screen.left, capture.left - kContextMargin);
  capture.top = std::max(virtual_screen.top, capture.top - kContextMargin);
  capture.right = std::min(virtual_screen.right, capture.right + kContextMargin);
  capture.bottom = std::min(virtual_screen.bottom, capture.bottom + kContextMargin);
  const int left = capture.left;
  const int top = capture.top;
  const int width = capture.right - capture.left;
  const int height = capture.bottom - capture.top;
  if (width <= 0 || height <= 0) return false;
  const uint64_t image_bytes = static_cast<uint64_t>(width) * height * 4;
  if (image_bytes > std::numeric_limits<UINT>::max()) return false;

  std::vector<uint8_t> pixels(static_cast<size_t>(image_bytes), 0);
  RenderGeometryOverlay(pixels.data(), width, height, width * 4, POINT{left, top},
                        highlight_rects);

  IWICImagingFactory* factory = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* encoder = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  IPropertyBag2* properties = nullptr;
  bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&factory)));
  if (ok) ok = SUCCEEDED(factory->CreateStream(&stream));
  if (ok) ok = SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
  if (ok) ok = SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
  if (ok) ok = SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache));
  if (ok) ok = SUCCEEDED(encoder->CreateNewFrame(&frame, &properties));
  if (ok) ok = SUCCEEDED(frame->Initialize(properties));
  if (ok) ok = SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)));
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (ok) ok = SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA;
  if (ok) {
    ok = SUCCEEDED(frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(width * 4),
                                      static_cast<UINT>(image_bytes), pixels.data()));
  }
  if (ok) ok = SUCCEEDED(frame->Commit());
  if (ok) ok = SUCCEEDED(encoder->Commit());

  SafeRelease(properties);
  SafeRelease(frame);
  SafeRelease(encoder);
  SafeRelease(stream);
  SafeRelease(factory);
  return ok;
}

}  // namespace azookey::compat_test
