#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <gtest/gtest.h>

#include "azookey/tsf/TextServiceFactory.h"

namespace {

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);

#define WIDEN_LITERAL2(value) L##value
#define WIDEN_LITERAL(value) WIDEN_LITERAL2(value)

}  // namespace

TEST(TsfTipComSmokeTest, DllGetClassObjectCreatesInstance) {
  HMODULE module = LoadLibraryW(WIDEN_LITERAL(AZOOKEY_TSF_TIP_DLL_PATH));
  ASSERT_NE(module, nullptr) << "LoadLibraryW failed";

  auto* proc = reinterpret_cast<DllGetClassObjectFn>(
      GetProcAddress(module, "DllGetClassObject"));
  ASSERT_NE(proc, nullptr) << "DllGetClassObject export not found";

  IClassFactory* factory = nullptr;
  HRESULT hr = proc(azookey::tsf::kTextServiceClsid, IID_IClassFactory,
                    reinterpret_cast<void**>(&factory));
  ASSERT_TRUE(SUCCEEDED(hr) && factory != nullptr) << "DllGetClassObject failed";

  IUnknown* service = nullptr;
  hr = factory->CreateInstance(nullptr, IID_IUnknown,
                               reinterpret_cast<void**>(&service));
  factory->Release();
  ASSERT_TRUE(SUCCEEDED(hr) && service != nullptr)
      << "CreateInstance(IID_IUnknown) failed";

  service->Release();
}
