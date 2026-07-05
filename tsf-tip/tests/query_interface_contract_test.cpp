#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <gtest/gtest.h>
#include <msctf.h>

#include <vector>

#include "azookey/tsf/CandidateListUIElement.h"
#include "azookey/tsf/DisplayAttribute.h"
#include "azookey/tsf/TextService.h"
#include "azookey/tsf/TextServiceFactory.h"

namespace {

template <typename T>
void ExpectNullOutParamReturnsPointer(T& object) {
  EXPECT_EQ(object.QueryInterface(IID_IUnknown, nullptr), E_POINTER);
}

template <typename T>
void ExpectUnsupportedInterfaceClearsOutParam(T& object) {
  void* out = &object;
  EXPECT_EQ(object.QueryInterface(IID_IDispatch, &out), E_NOINTERFACE);
  EXPECT_EQ(out, nullptr);
}

}  // namespace

TEST(TsfTipQueryInterfaceContractTest, NullOutParamReturnsPointer) {
  azookey::tsf::TextService service;
  azookey::tsf::EditSession edit_session(&service, nullptr);
  azookey::tsf::TextServiceFactory factory;
  azookey::tsf::InputDisplayAttributeInfo input_attribute;
  azookey::tsf::EnumDisplayAttributeInfo attribute_enumerator;
  azookey::tsf::CandidateListUIElement candidates({L"candidate"}, 0);

  ExpectNullOutParamReturnsPointer(service);
  ExpectNullOutParamReturnsPointer(edit_session);
  ExpectNullOutParamReturnsPointer(factory);
  ExpectNullOutParamReturnsPointer(input_attribute);
  ExpectNullOutParamReturnsPointer(attribute_enumerator);
  ExpectNullOutParamReturnsPointer(candidates);
}

TEST(TsfTipQueryInterfaceContractTest, UnsupportedInterfaceClearsOutParam) {
  azookey::tsf::TextService service;
  azookey::tsf::EditSession edit_session(&service, nullptr);
  azookey::tsf::TextServiceFactory factory;
  azookey::tsf::InputDisplayAttributeInfo input_attribute;
  azookey::tsf::EnumDisplayAttributeInfo attribute_enumerator;
  azookey::tsf::CandidateListUIElement candidates({L"candidate"}, 0);

  ExpectUnsupportedInterfaceClearsOutParam(service);
  ExpectUnsupportedInterfaceClearsOutParam(edit_session);
  ExpectUnsupportedInterfaceClearsOutParam(factory);
  ExpectUnsupportedInterfaceClearsOutParam(input_attribute);
  ExpectUnsupportedInterfaceClearsOutParam(attribute_enumerator);
  ExpectUnsupportedInterfaceClearsOutParam(candidates);
}
