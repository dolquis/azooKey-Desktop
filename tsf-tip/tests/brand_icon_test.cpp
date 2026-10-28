#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <gtest/gtest.h>
#include <shellapi.h>

#include "resource.h"

namespace {

#define WIDEN_LITERAL2(value) L##value
#define WIDEN_LITERAL(value) WIDEN_LITERAL2(value)

constexpr const wchar_t* kTipDllPath = WIDEN_LITERAL(AZOOKEY_TSF_TIP_DLL_PATH);

// RegisterProfile / AddLanguageProfile pass the DLL path with uIconIndex 0,
// so the first icon group in the DLL is the brand icon TSF shows (DEV-1744).
TEST(TsfTipBrandIconTest, DllHasExactlyOneIconGroup) {
  EXPECT_EQ(ExtractIconExW(kTipDllPath, -1, nullptr, nullptr, 0), 1u);
}

TEST(TsfTipBrandIconTest, ProfileIconIndexZeroYieldsLargeAndSmallIcons) {
  HICON large_icon = nullptr;
  HICON small_icon = nullptr;
  ASSERT_EQ(ExtractIconExW(kTipDllPath, 0, &large_icon, &small_icon, 1), 2u);
  ASSERT_NE(large_icon, nullptr);
  ASSERT_NE(small_icon, nullptr);
  DestroyIcon(large_icon);
  DestroyIcon(small_icon);
}

TEST(TsfTipBrandIconTest, IconGroupProvidesSixteenPixelImage) {
  HMODULE module = LoadLibraryExW(kTipDllPath, nullptr,
                                  LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  ASSERT_NE(module, nullptr);
  HICON icon = static_cast<HICON>(
      LoadImageW(module, MAKEINTRESOURCEW(IDI_AZOOKEY_BRAND), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
  ASSERT_NE(icon, nullptr);
  ICONINFO info = {};
  ASSERT_TRUE(GetIconInfo(icon, &info));
  BITMAP bitmap = {};
  ASSERT_NE(GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap), 0);
  EXPECT_EQ(bitmap.bmWidth, 16);
  EXPECT_EQ(bitmap.bmHeight, 16);
  DeleteObject(info.hbmColor);
  DeleteObject(info.hbmMask);
  DestroyIcon(icon);
  FreeLibrary(module);
}

}  // namespace
