#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <OleAuto.h>

#include <gtest/gtest.h>
#include <msctf.h>

#include "azookey/tsf/DisplayAttribute.h"
#include "azookey/tsf/TextService.h"

namespace {

void ExpectInputAttributeGuid(ITfDisplayAttributeInfo* info) {
  ASSERT_NE(info, nullptr);
  GUID guid{};
  ASSERT_EQ(info->GetGUID(&guid), S_OK);
  EXPECT_TRUE(IsEqualGUID(guid, azookey::tsf::kInputAttributeGuid));
}

}  // namespace

TEST(TsfTipDisplayAttributeTest, TextServiceResolvesInputAttributeGuid) {
  azookey::tsf::TextService service;

  ITfDisplayAttributeInfo* info = nullptr;
  EXPECT_EQ(service.GetDisplayAttributeInfo(azookey::tsf::kInputAttributeGuid, &info), S_OK);
  ASSERT_NO_FATAL_FAILURE(ExpectInputAttributeGuid(info));
  info->Release();

  info = nullptr;
  EXPECT_EQ(service.GetDisplayAttributeInfo(GUID_NULL, &info), E_INVALIDARG);
  EXPECT_EQ(info, nullptr);
  EXPECT_EQ(service.GetDisplayAttributeInfo(azookey::tsf::kInputAttributeGuid, nullptr),
            E_INVALIDARG);
}

TEST(TsfTipDisplayAttributeTest, TextServiceEnumeratesInputAttributeAndResets) {
  azookey::tsf::TextService service;

  IEnumTfDisplayAttributeInfo* enumerator = nullptr;
  ASSERT_EQ(service.EnumDisplayAttributeInfo(&enumerator), S_OK);
  ASSERT_NE(enumerator, nullptr);

  ITfDisplayAttributeInfo* infos[2] = {};
  ULONG fetched = 0;
  EXPECT_EQ(enumerator->Next(2, infos, &fetched), S_FALSE);
  EXPECT_EQ(fetched, 1u);
  ASSERT_NO_FATAL_FAILURE(ExpectInputAttributeGuid(infos[0]));
  EXPECT_EQ(infos[1], nullptr);
  infos[0]->Release();

  fetched = 999;
  infos[0] = nullptr;
  EXPECT_EQ(enumerator->Next(1, infos, &fetched), S_FALSE);
  EXPECT_EQ(fetched, 0u);
  EXPECT_EQ(infos[0], nullptr);

  ASSERT_EQ(enumerator->Reset(), S_OK);
  fetched = 0;
  EXPECT_EQ(enumerator->Next(1, infos, &fetched), S_OK);
  EXPECT_EQ(fetched, 1u);
  ASSERT_NO_FATAL_FAILURE(ExpectInputAttributeGuid(infos[0]));
  infos[0]->Release();

  enumerator->Release();
  EXPECT_EQ(service.EnumDisplayAttributeInfo(nullptr), E_INVALIDARG);
}

TEST(TsfTipDisplayAttributeTest, InputAttributeInfoReturnsUnderlineDefinition) {
  azookey::tsf::InputDisplayAttributeInfo info;

  GUID guid{};
  EXPECT_EQ(info.GetGUID(&guid), S_OK);
  EXPECT_TRUE(IsEqualGUID(guid, azookey::tsf::kInputAttributeGuid));
  EXPECT_EQ(info.GetGUID(nullptr), E_INVALIDARG);

  BSTR description = nullptr;
  ASSERT_EQ(info.GetDescription(&description), S_OK);
  ASSERT_NE(description, nullptr);
  EXPECT_STREQ(description, L"azooKey Input");
  SysFreeString(description);
  EXPECT_EQ(info.GetDescription(nullptr), E_INVALIDARG);

  TF_DISPLAYATTRIBUTE attr{};
  EXPECT_EQ(info.GetAttributeInfo(&attr), S_OK);
  EXPECT_EQ(attr.crText.type, TF_CT_NONE);
  EXPECT_EQ(attr.crBk.type, TF_CT_NONE);
  EXPECT_EQ(attr.crLine.type, TF_CT_NONE);
  EXPECT_EQ(attr.lsStyle, TF_LS_SOLID);
  EXPECT_EQ(attr.fBoldLine, FALSE);
  EXPECT_EQ(attr.bAttr, TF_ATTR_INPUT);

  EXPECT_EQ(info.GetAttributeInfo(nullptr), E_INVALIDARG);
  EXPECT_EQ(info.SetAttributeInfo(&attr), E_NOTIMPL);
  EXPECT_EQ(info.Reset(), S_OK);
}
