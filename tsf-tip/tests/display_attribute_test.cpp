#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <Windows.h>
#include <OleAuto.h>
// clang-format on
#include <gtest/gtest.h>
#include <msctf.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "azookey/tsf/DisplayAttribute.h"
#include "azookey/tsf/TextService.h"

namespace {

constexpr ULONG kAttributeCount = azookey::tsf::kDisplayAttributeKindCount;

void ExpectAttributeGuid(ITfDisplayAttributeInfo* info, const GUID& expected) {
  ASSERT_NE(info, nullptr);
  GUID guid{};
  ASSERT_EQ(info->GetGUID(&guid), S_OK);
  EXPECT_TRUE(IsEqualGUID(guid, expected));
}

void ExpectInputAttributeGuid(ITfDisplayAttributeInfo* info) {
  ExpectAttributeGuid(info, azookey::tsf::kInputAttributeGuid);
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

  // The input underline comes first so a single-attribute consumer still finds it.
  ITfDisplayAttributeInfo* infos[kAttributeCount + 1] = {};
  ULONG fetched = 0;
  EXPECT_EQ(enumerator->Next(kAttributeCount + 1, infos, &fetched), S_FALSE);
  EXPECT_EQ(fetched, kAttributeCount);
  ASSERT_NO_FATAL_FAILURE(ExpectInputAttributeGuid(infos[0]));
  ASSERT_NO_FATAL_FAILURE(
      ExpectAttributeGuid(infos[1], azookey::tsf::kFocusedSegmentAttributeGuid));
  ASSERT_NO_FATAL_FAILURE(
      ExpectAttributeGuid(infos[2], azookey::tsf::kConvertedSegmentAttributeGuid));
  ASSERT_NO_FATAL_FAILURE(
      ExpectAttributeGuid(infos[3], azookey::tsf::kUnconvertedSegmentAttributeGuid));
  EXPECT_EQ(infos[kAttributeCount], nullptr);
  for (ULONG i = 0; i < kAttributeCount; ++i) infos[i]->Release();

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

  EXPECT_EQ(enumerator->Skip(kAttributeCount), S_OK);

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
  fetched = 0;
  ASSERT_EQ(clone->Next(1, &info, &fetched), S_OK);
  EXPECT_EQ(fetched, 1u);
  ASSERT_NO_FATAL_FAILURE(ExpectAttributeGuid(info, azookey::tsf::kFocusedSegmentAttributeGuid));
  info->Release();

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
  ITfDisplayAttributeInfo* infos[kAttributeCount] = {};
  ULONG fetched = 0;
  ASSERT_EQ(clone->Next(kAttributeCount, infos, &fetched), S_OK);
  EXPECT_EQ(fetched, kAttributeCount);
  for (auto* drained : infos) drained->Release();

  ITfDisplayAttributeInfo* info = nullptr;
  fetched = 999;
  EXPECT_EQ(clone->Next(1, &info, &fetched), S_FALSE);
  EXPECT_EQ(fetched, 0u);
  EXPECT_EQ(info, nullptr);

  fetched = 0;
  ASSERT_EQ(enumerator->Next(kAttributeCount, infos, &fetched), S_OK);
  EXPECT_EQ(fetched, kAttributeCount);
  ASSERT_NO_FATAL_FAILURE(ExpectInputAttributeGuid(infos[0]));
  for (auto* drained : infos) drained->Release();

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

  ASSERT_EQ(enumerator->Skip(kAttributeCount), S_OK);

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
  azookey::tsf::DisplayAttributeInfo info;

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

namespace {

using azookey::tsf::DisplayAttributeKind;
using azookey::tsf::DisplayedSegment;
using azookey::tsf::SegmentAttributeRange;

// Range over [start, end) of a composition of `limit` characters. Clones are
// owned by the composition so the test can inspect them afterwards.
class OffsetRange final : public ITfRange {
 public:
  OffsetRange(LONG start, LONG end, LONG limit, std::vector<std::unique_ptr<OffsetRange>>* clones)
      : start_(start), end_(end), limit_(limit), clones_(clones) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid != IID_IUnknown && riid != IID_ITfRange) return E_NOINTERFACE;
    *object = static_cast<ITfRange*>(this);
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 1; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }
  STDMETHODIMP GetText(TfEditCookie, DWORD, WCHAR*, ULONG, ULONG*) override { return E_NOTIMPL; }
  STDMETHODIMP SetText(TfEditCookie, DWORD, const WCHAR*, LONG) override { return E_NOTIMPL; }
  STDMETHODIMP GetFormattedText(TfEditCookie, IDataObject**) override { return E_NOTIMPL; }
  STDMETHODIMP GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown**) override { return E_NOTIMPL; }
  STDMETHODIMP InsertEmbedded(TfEditCookie, DWORD, IDataObject*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStart(TfEditCookie, LONG count, LONG* shifted, const TF_HALTCOND*) override {
    const LONG target = std::clamp(start_ + count, 0L, limit_);
    if (shifted) *shifted = target - start_;
    start_ = target;
    if (end_ < start_) end_ = start_;
    return S_OK;
  }
  STDMETHODIMP ShiftEnd(TfEditCookie, LONG count, LONG* shifted, const TF_HALTCOND*) override {
    if (fail_shift) return E_FAIL;
    const LONG target = std::clamp(end_ + count, 0L, limit_);
    if (shifted) *shifted = target - end_;
    end_ = target;
    if (start_ > end_) start_ = end_;
    return S_OK;
  }
  STDMETHODIMP ShiftStartToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftEndToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStartRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftEndRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP IsEmpty(TfEditCookie, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP Collapse(TfEditCookie, TfAnchor anchor) override {
    if (anchor == TF_ANCHOR_START)
      end_ = start_;
    else
      start_ = end_;
    return S_OK;
  }
  STDMETHODIMP IsEqualStart(TfEditCookie, ITfRange*, TfAnchor, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP IsEqualEnd(TfEditCookie, ITfRange*, TfAnchor, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP CompareStart(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
  STDMETHODIMP CompareEnd(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
  STDMETHODIMP AdjustForInsert(TfEditCookie, ULONG, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP GetGravity(TfGravity*, TfGravity*) override { return E_NOTIMPL; }
  STDMETHODIMP SetGravity(TfEditCookie, TfGravity, TfGravity) override { return E_NOTIMPL; }
  STDMETHODIMP Clone(ITfRange** clone) override {
    if (!clone) return E_POINTER;
    clones_->push_back(std::make_unique<OffsetRange>(start_, end_, limit_, clones_));
    clones_->back()->fail_shift = fail_shift_in_clones;
    *clone = clones_->back().get();
    return S_OK;
  }
  STDMETHODIMP GetContext(ITfContext**) override { return E_NOTIMPL; }

  LONG start() const { return start_; }
  LONG end() const { return end_; }
  bool fail_shift{false};
  bool fail_shift_in_clones{false};

 private:
  LONG start_;
  LONG end_;
  LONG limit_;
  std::vector<std::unique_ptr<OffsetRange>>* clones_;
};

struct AttributeWrite {
  LONG start;
  LONG end;
  TfGuidAtom atom;
};

class RecordingProperty final : public ITfProperty {
 public:
  STDMETHODIMP QueryInterface(REFIID, void** object) override {
    if (object) *object = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 1; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }
  STDMETHODIMP GetType(GUID*) override { return E_NOTIMPL; }
  STDMETHODIMP EnumRanges(TfEditCookie, IEnumTfRanges**, ITfRange*) override { return E_NOTIMPL; }
  STDMETHODIMP GetValue(TfEditCookie, ITfRange*, VARIANT*) override { return E_NOTIMPL; }
  STDMETHODIMP GetContext(ITfContext**) override { return E_NOTIMPL; }
  STDMETHODIMP FindRange(TfEditCookie, ITfRange*, ITfRange**, TfAnchor) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP SetValueStore(TfEditCookie, ITfRange*, ITfPropertyStore*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP SetValue(TfEditCookie, ITfRange* range, const VARIANT* value) override {
    auto* offsets = static_cast<OffsetRange*>(range);
    if (value->vt != VT_I4) return E_INVALIDARG;
    writes.push_back({offsets->start(), offsets->end(), static_cast<TfGuidAtom>(value->lVal)});
    return S_OK;
  }
  STDMETHODIMP Clear(TfEditCookie, ITfRange*) override { return E_NOTIMPL; }

  std::vector<AttributeWrite> writes;
};

constexpr TfGuidAtom kAtoms[azookey::tsf::kDisplayAttributeKindCount] = {11, 12, 13, 14};

}  // namespace

TEST(TsfTipDisplayAttributeTest, SegmentAttributesDifferByAttributeAndUnderlineOnly) {
  const auto focused =
      azookey::tsf::DisplayAttributeDefinition(DisplayAttributeKind::FocusedSegment);
  const auto converted =
      azookey::tsf::DisplayAttributeDefinition(DisplayAttributeKind::ConvertedSegment);
  const auto unconverted =
      azookey::tsf::DisplayAttributeDefinition(DisplayAttributeKind::UnconvertedSegment);

  EXPECT_EQ(focused.bAttr, TF_ATTR_TARGET_CONVERTED);
  EXPECT_EQ(focused.lsStyle, TF_LS_SOLID);
  EXPECT_EQ(focused.fBoldLine, TRUE);
  EXPECT_EQ(converted.bAttr, TF_ATTR_CONVERTED);
  EXPECT_EQ(converted.lsStyle, TF_LS_SOLID);
  EXPECT_EQ(converted.fBoldLine, FALSE);
  EXPECT_EQ(unconverted.bAttr, TF_ATTR_INPUT);
  EXPECT_EQ(unconverted.lsStyle, TF_LS_DOT);
  for (const auto& attr : {focused, converted, unconverted}) {
    EXPECT_EQ(attr.crText.type, TF_CT_NONE);
    EXPECT_EQ(attr.crBk.type, TF_CT_NONE);
    EXPECT_EQ(attr.crLine.type, TF_CT_NONE);
  }
}

TEST(TsfTipDisplayAttributeTest, TextServiceResolvesEverySegmentAttributeGuid) {
  azookey::tsf::TextService service;

  for (const GUID* guid :
       {&azookey::tsf::kFocusedSegmentAttributeGuid, &azookey::tsf::kConvertedSegmentAttributeGuid,
        &azookey::tsf::kUnconvertedSegmentAttributeGuid}) {
    ITfDisplayAttributeInfo* info = nullptr;
    ASSERT_EQ(service.GetDisplayAttributeInfo(*guid, &info), S_OK);
    ASSERT_NO_FATAL_FAILURE(ExpectAttributeGuid(info, *guid));
    BSTR description = nullptr;
    ASSERT_EQ(info->GetDescription(&description), S_OK);
    EXPECT_NE(std::wstring(description), L"azooKey Input");
    SysFreeString(description);
    info->Release();
  }
}

TEST(TsfTipDisplayAttributeTest, SegmentRangesFollowTheFocusAndWhatEachSegmentShows) {
  const std::vector<DisplayedSegment> segments{{2, true}, {0, true}, {3, false}, {1, true}};

  const auto ranges = azookey::tsf::BuildSegmentAttributeRanges(segments, 3);

  ASSERT_EQ(ranges.size(), 3u);
  EXPECT_EQ(ranges[0].start, 0);
  EXPECT_EQ(ranges[0].length, 2);
  EXPECT_EQ(ranges[0].kind, DisplayAttributeKind::ConvertedSegment);
  EXPECT_EQ(ranges[1].start, 2);
  EXPECT_EQ(ranges[1].length, 3);
  EXPECT_EQ(ranges[1].kind, DisplayAttributeKind::UnconvertedSegment);
  EXPECT_EQ(ranges[2].start, 5);
  EXPECT_EQ(ranges[2].kind, DisplayAttributeKind::FocusedSegment);
  EXPECT_TRUE(azookey::tsf::BuildSegmentAttributeRanges({}, 0).empty());
}

TEST(TsfTipDisplayAttributeTest, MouseEdgeAndQuadrantPickTheSegmentUnderTheClick) {
  const std::vector<DisplayedSegment> segments{{2, true}, {0, true}, {3, true}};
  size_t index = 99;

  // Quadrants 0 and 1 precede the edge, so edge 2 then lies on the first segment.
  ASSERT_TRUE(azookey::tsf::SegmentIndexAtMouseEdge(segments, 2, 1, &index));
  EXPECT_EQ(index, 0u);
  ASSERT_TRUE(azookey::tsf::SegmentIndexAtMouseEdge(segments, 2, 2, &index));
  EXPECT_EQ(index, 2u);
  ASSERT_TRUE(azookey::tsf::SegmentIndexAtMouseEdge(segments, 0, 0, &index));
  EXPECT_EQ(index, 0u);
  // Past the end of the composition clamps to the last segment.
  ASSERT_TRUE(azookey::tsf::SegmentIndexAtMouseEdge(segments, 9, 3, &index));
  EXPECT_EQ(index, 2u);
  EXPECT_FALSE(azookey::tsf::SegmentIndexAtMouseEdge({{0, true}}, 0, 2, &index));
}

TEST(TsfTipDisplayAttributeTest, AppliesEachSegmentAttributeToItsOwnRange) {
  std::vector<std::unique_ptr<OffsetRange>> clones;
  OffsetRange composition(0, 5, 5, &clones);
  RecordingProperty property;
  const std::vector<SegmentAttributeRange> ranges{
      {0, 2, DisplayAttributeKind::FocusedSegment},
      {2, 3, DisplayAttributeKind::UnconvertedSegment},
  };

  ASSERT_EQ(azookey::tsf::ApplyDisplayAttributes(1, &property, &composition, ranges, kAtoms), S_OK);

  ASSERT_EQ(property.writes.size(), 2u);
  EXPECT_EQ(property.writes[0].start, 0);
  EXPECT_EQ(property.writes[0].end, 2);
  EXPECT_EQ(property.writes[0].atom, 12u);
  EXPECT_EQ(property.writes[1].start, 2);
  EXPECT_EQ(property.writes[1].end, 5);
  EXPECT_EQ(property.writes[1].atom, 14u);
  // The composition range itself stays whole for the selection update.
  EXPECT_EQ(composition.start(), 0);
  EXPECT_EQ(composition.end(), 5);
}

TEST(TsfTipDisplayAttributeTest, FallsBackToTheInputAttributeOverTheWholeComposition) {
  std::vector<std::unique_ptr<OffsetRange>> clones;
  OffsetRange composition(0, 5, 5, &clones);
  RecordingProperty property;

  ASSERT_EQ(azookey::tsf::ApplyDisplayAttributes(1, &property, &composition, {}, kAtoms), S_OK);
  ASSERT_EQ(property.writes.size(), 1u);
  EXPECT_EQ(property.writes[0].start, 0);
  EXPECT_EQ(property.writes[0].end, 5);
  EXPECT_EQ(property.writes[0].atom, 11u);

  // A segment that cannot be isolated repaints everything with the fallback.
  property.writes.clear();
  composition.fail_shift_in_clones = true;
  ASSERT_EQ(azookey::tsf::ApplyDisplayAttributes(
                1, &property, &composition, {{0, 2, DisplayAttributeKind::FocusedSegment}}, kAtoms),
            S_OK);
  ASSERT_EQ(property.writes.size(), 1u);
  EXPECT_EQ(property.writes[0].end, 5);
  EXPECT_EQ(property.writes[0].atom, 11u);

  // A missing segment atom also falls back.
  property.writes.clear();
  composition.fail_shift_in_clones = false;
  const TfGuidAtom missing[azookey::tsf::kDisplayAttributeKindCount] = {11, 0, 13, 14};
  ASSERT_EQ(
      azookey::tsf::ApplyDisplayAttributes(1, &property, &composition,
                                           {{0, 5, DisplayAttributeKind::FocusedSegment}}, missing),
      S_OK);
  ASSERT_EQ(property.writes.size(), 1u);
  EXPECT_EQ(property.writes[0].atom, 11u);
}
