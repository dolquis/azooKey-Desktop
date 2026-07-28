#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <Windows.h>
#include <OleAuto.h>
// clang-format on
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

void ExpectSameColor(const TF_DA_COLOR& lhs, const TF_DA_COLOR& rhs) {
  ASSERT_EQ(lhs.type, rhs.type);
  switch (lhs.type) {
    case TF_CT_SYSCOLOR:
      EXPECT_EQ(lhs.nIndex, rhs.nIndex);
      break;
    case TF_CT_COLORREF:
      EXPECT_EQ(lhs.cr, rhs.cr);
      break;
    default:
      // TF_CT_NONE carries no payload; the type alone defines the value.
      break;
  }
}

// The TSF manager may resolve a TfGuidAtom back to its definition through a
// cloned enumerator, so a clone must yield the same attribute definition as the
// source rather than a default-constructed one.
void ExpectSameAttributeDefinition(ITfDisplayAttributeInfo* lhs, ITfDisplayAttributeInfo* rhs) {
  ASSERT_NE(lhs, nullptr);
  ASSERT_NE(rhs, nullptr);

  GUID lhs_guid{};
  GUID rhs_guid{};
  ASSERT_EQ(lhs->GetGUID(&lhs_guid), S_OK);
  ASSERT_EQ(rhs->GetGUID(&rhs_guid), S_OK);
  EXPECT_TRUE(IsEqualGUID(lhs_guid, rhs_guid));

  TF_DISPLAYATTRIBUTE lhs_attr{};
  TF_DISPLAYATTRIBUTE rhs_attr{};
  ASSERT_EQ(lhs->GetAttributeInfo(&lhs_attr), S_OK);
  ASSERT_EQ(rhs->GetAttributeInfo(&rhs_attr), S_OK);
  ASSERT_NO_FATAL_FAILURE(ExpectSameColor(lhs_attr.crText, rhs_attr.crText));
  ASSERT_NO_FATAL_FAILURE(ExpectSameColor(lhs_attr.crBk, rhs_attr.crBk));
  ASSERT_NO_FATAL_FAILURE(ExpectSameColor(lhs_attr.crLine, rhs_attr.crLine));
  EXPECT_EQ(lhs_attr.lsStyle, rhs_attr.lsStyle);
  EXPECT_EQ(lhs_attr.fBoldLine, rhs_attr.fBoldLine);
  EXPECT_EQ(lhs_attr.bAttr, rhs_attr.bAttr);
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

TEST(TsfTipDisplayAttributeTest, DisplayAttributeEnumeratorSkipsToEnd) {
  azookey::tsf::TextService service;

  IEnumTfDisplayAttributeInfo* enumerator = nullptr;
  ASSERT_EQ(service.EnumDisplayAttributeInfo(&enumerator), S_OK);
  ASSERT_NE(enumerator, nullptr);

  EXPECT_EQ(enumerator->Skip(1), S_OK);

  ITfDisplayAttributeInfo* info = nullptr;
  ULONG fetched = 999;
  EXPECT_EQ(enumerator->Next(1, &info, &fetched), S_FALSE);
  EXPECT_EQ(fetched, 0u);
  EXPECT_EQ(info, nullptr);

  enumerator->Release();
}

TEST(TsfTipDisplayAttributeTest, DisplayAttributeEnumeratorCloneKeepsCurrentPosition) {
  azookey::tsf::TextService service;

  IEnumTfDisplayAttributeInfo* enumerator = nullptr;
  ASSERT_EQ(service.EnumDisplayAttributeInfo(&enumerator), S_OK);
  ASSERT_NE(enumerator, nullptr);

  ITfDisplayAttributeInfo* info = nullptr;
  ULONG fetched = 0;
  ASSERT_EQ(enumerator->Next(1, &info, &fetched), S_OK);
  EXPECT_EQ(fetched, 1u);
  info->Release();

  IEnumTfDisplayAttributeInfo* clone = nullptr;
  ASSERT_EQ(enumerator->Clone(&clone), S_OK);
  ASSERT_NE(clone, nullptr);

  info = nullptr;
  fetched = 999;
  EXPECT_EQ(clone->Next(1, &info, &fetched), S_FALSE);
  EXPECT_EQ(fetched, 0u);
  EXPECT_EQ(info, nullptr);

  clone->Release();
  enumerator->Release();
}

TEST(TsfTipDisplayAttributeTest, DisplayAttributeEnumeratorCloneCopiesAttributeDefinition) {
  azookey::tsf::TextService service;

  IEnumTfDisplayAttributeInfo* enumerator = nullptr;
  ASSERT_EQ(service.EnumDisplayAttributeInfo(&enumerator), S_OK);
  ASSERT_NE(enumerator, nullptr);

  IEnumTfDisplayAttributeInfo* clone = nullptr;
  ASSERT_EQ(enumerator->Clone(&clone), S_OK);
  ASSERT_NE(clone, nullptr);

  ITfDisplayAttributeInfo* source_info = nullptr;
  ULONG fetched = 0;
  ASSERT_EQ(enumerator->Next(1, &source_info, &fetched), S_OK);
  ASSERT_EQ(fetched, 1u);

  ITfDisplayAttributeInfo* clone_info = nullptr;
  fetched = 0;
  ASSERT_EQ(clone->Next(1, &clone_info, &fetched), S_OK);
  ASSERT_EQ(fetched, 1u);

  // The clone yields a distinct COM object carrying the same definition.
  EXPECT_NE(clone_info, source_info);
  ASSERT_NO_FATAL_FAILURE(ExpectSameAttributeDefinition(source_info, clone_info));
  ASSERT_NO_FATAL_FAILURE(ExpectInputAttributeGuid(clone_info));

  clone_info->Release();
  source_info->Release();
  clone->Release();
  enumerator->Release();
}

TEST(TsfTipDisplayAttributeTest, DisplayAttributeEnumeratorCloneAdvancesIndependently) {
  azookey::tsf::TextService service;

  IEnumTfDisplayAttributeInfo* enumerator = nullptr;
  ASSERT_EQ(service.EnumDisplayAttributeInfo(&enumerator), S_OK);
  ASSERT_NE(enumerator, nullptr);

  IEnumTfDisplayAttributeInfo* clone = nullptr;
  ASSERT_EQ(enumerator->Clone(&clone), S_OK);
  ASSERT_NE(clone, nullptr);

  // Draining the clone must not move the source cursor.
  ITfDisplayAttributeInfo* info = nullptr;
  ULONG fetched = 0;
  ASSERT_EQ(clone->Next(1, &info, &fetched), S_OK);
  EXPECT_EQ(fetched, 1u);
  info->Release();

  info = nullptr;
  fetched = 999;
  EXPECT_EQ(clone->Next(1, &info, &fetched), S_FALSE);
  EXPECT_EQ(fetched, 0u);
  EXPECT_EQ(info, nullptr);

  fetched = 0;
  ASSERT_EQ(enumerator->Next(1, &info, &fetched), S_OK);
  EXPECT_EQ(fetched, 1u);
  ASSERT_NO_FATAL_FAILURE(ExpectInputAttributeGuid(info));
  info->Release();

  // Resetting the clone must not rewind the exhausted source either.
  ASSERT_EQ(clone->Reset(), S_OK);
  info = nullptr;
  fetched = 999;
  EXPECT_EQ(enumerator->Next(1, &info, &fetched), S_FALSE);
  EXPECT_EQ(fetched, 0u);
  EXPECT_EQ(info, nullptr);

  clone->Release();
  enumerator->Release();
}

TEST(TsfTipDisplayAttributeTest, DisplayAttributeEnumeratorClonePreservesSkipOffset) {
  azookey::tsf::TextService service;

  IEnumTfDisplayAttributeInfo* enumerator = nullptr;
  ASSERT_EQ(service.EnumDisplayAttributeInfo(&enumerator), S_OK);
  ASSERT_NE(enumerator, nullptr);

  ASSERT_EQ(enumerator->Skip(1), S_OK);

  IEnumTfDisplayAttributeInfo* clone = nullptr;
  ASSERT_EQ(enumerator->Clone(&clone), S_OK);
  ASSERT_NE(clone, nullptr);

  ITfDisplayAttributeInfo* info = nullptr;
  ULONG fetched = 999;
  EXPECT_EQ(clone->Next(1, &info, &fetched), S_FALSE);
  EXPECT_EQ(fetched, 0u);
  EXPECT_EQ(info, nullptr);

  // A reset clone re-enumerates from the start of the attribute list.
  ASSERT_EQ(clone->Reset(), S_OK);
  fetched = 0;
  ASSERT_EQ(clone->Next(1, &info, &fetched), S_OK);
  EXPECT_EQ(fetched, 1u);
  ASSERT_NO_FATAL_FAILURE(ExpectInputAttributeGuid(info));
  info->Release();

  EXPECT_EQ(clone->Clone(nullptr), E_INVALIDARG);

  clone->Release();
  enumerator->Release();
}

TEST(TsfTipDisplayAttributeTest, DisplayAttributeAllocationFailuresReturnOutOfMemory) {
  azookey::tsf::TextService service;

  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  IEnumTfDisplayAttributeInfo* enumerator = nullptr;
  EXPECT_EQ(service.EnumDisplayAttributeInfo(&enumerator), E_OUTOFMEMORY);
  EXPECT_EQ(enumerator, nullptr);

  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  ITfDisplayAttributeInfo* info = nullptr;
  EXPECT_EQ(service.GetDisplayAttributeInfo(azookey::tsf::kInputAttributeGuid, &info),
            E_OUTOFMEMORY);
  EXPECT_EQ(info, nullptr);

  azookey::tsf::EnumDisplayAttributeInfo local_enumerator;
  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  ULONG fetched = 999;
  EXPECT_EQ(local_enumerator.Next(1, &info, &fetched), E_OUTOFMEMORY);
  EXPECT_EQ(fetched, 0u);
  EXPECT_EQ(info, nullptr);

  azookey::tsf::testing::FailNextComBoundaryAllocationForTest();
  EXPECT_EQ(local_enumerator.Clone(&enumerator), E_OUTOFMEMORY);
  EXPECT_EQ(enumerator, nullptr);
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
