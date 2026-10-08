#include "azookey/tsf/DisplayAttribute.h"

#include <new>
#include <utility>

#include "azookey/tsf/TipRuntimeLog.h"

namespace {

template <typename T, typename... Args>
T* NewComBoundaryObject(Args&&... args) {
#ifdef AZOOKEY_TSF_TESTING
  if (azookey::tsf::testing::ConsumeComBoundaryAllocationFailureForTest()) {
    return nullptr;
  }
#endif
  return new (std::nothrow) T(std::forward<Args>(args)...);
}

}  // namespace

namespace azookey::tsf {

const GUID& DisplayAttributeGuid(DisplayAttributeKind kind) {
  switch (kind) {
    case DisplayAttributeKind::FocusedSegment:
      return kFocusedSegmentAttributeGuid;
    case DisplayAttributeKind::ConvertedSegment:
      return kConvertedSegmentAttributeGuid;
    case DisplayAttributeKind::UnconvertedSegment:
      return kUnconvertedSegmentAttributeGuid;
    case DisplayAttributeKind::Input:
      break;
  }
  return kInputAttributeGuid;
}

TF_DISPLAYATTRIBUTE DisplayAttributeDefinition(DisplayAttributeKind kind) {
  TF_DISPLAYATTRIBUTE attribute{};
  attribute.crText.type = TF_CT_NONE;
  attribute.crBk.type = TF_CT_NONE;
  attribute.crLine.type = TF_CT_NONE;
  attribute.lsStyle = TF_LS_SOLID;
  attribute.fBoldLine = FALSE;
  attribute.bAttr = TF_ATTR_INPUT;
  switch (kind) {
    case DisplayAttributeKind::FocusedSegment:
      attribute.fBoldLine = TRUE;
      attribute.bAttr = TF_ATTR_TARGET_CONVERTED;
      break;
    case DisplayAttributeKind::ConvertedSegment:
      attribute.bAttr = TF_ATTR_CONVERTED;
      break;
    case DisplayAttributeKind::UnconvertedSegment:
      attribute.lsStyle = TF_LS_DOT;
      break;
    case DisplayAttributeKind::Input:
      break;
  }
  return attribute;
}

bool FindDisplayAttributeKind(REFGUID guid, DisplayAttributeKind* kind) {
  for (ULONG index = 0; index < kDisplayAttributeKindCount; ++index) {
    const auto candidate = static_cast<DisplayAttributeKind>(index);
    if (IsEqualGUID(guid, DisplayAttributeGuid(candidate))) {
      if (kind) *kind = candidate;
      return true;
    }
  }
  return false;
}

std::vector<SegmentAttributeRange> BuildSegmentAttributeRanges(
    const std::vector<DisplayedSegment>& segments, size_t focused_index) {
  std::vector<SegmentAttributeRange> ranges;
  LONG start = 0;
  for (size_t index = 0; index < segments.size(); ++index) {
    const DisplayedSegment& segment = segments[index];
    if (segment.length <= 0) continue;
    const DisplayAttributeKind kind = index == focused_index ? DisplayAttributeKind::FocusedSegment
                                      : segment.converted
                                          ? DisplayAttributeKind::ConvertedSegment
                                          : DisplayAttributeKind::UnconvertedSegment;
    ranges.push_back({start, segment.length, kind});
    start += segment.length;
  }
  return ranges;
}

bool SegmentIndexAtMouseEdge(const std::vector<DisplayedSegment>& segments, ULONG edge,
                             ULONG quadrant, size_t* index) {
  // Quadrants 0 and 1 lie before the edge, so they belong to the preceding character.
  const ULONG character = quadrant < 2 && edge > 0 ? edge - 1 : edge;
  bool found = false;
  ULONG start = 0;
  for (size_t i = 0; i < segments.size(); ++i) {
    if (segments[i].length <= 0) continue;
    found = true;
    if (index) *index = i;
    start += static_cast<ULONG>(segments[i].length);
    if (character < start) break;
  }
  return found;
}

namespace {

HRESULT SetAttributeValue(TfEditCookie ec, ITfProperty* property, ITfRange* range,
                          TfGuidAtom atom) {
  if (atom == TF_INVALID_GUIDATOM) return E_FAIL;
  VARIANT value;
  VariantInit(&value);
  value.vt = VT_I4;
  value.lVal = static_cast<LONG>(atom);
  return property->SetValue(ec, range, &value);
}

HRESULT SetSegmentAttribute(TfEditCookie ec, ITfProperty* property, ITfRange* composition,
                            const SegmentAttributeRange& segment, TfGuidAtom atom) {
  ITfRange* range = nullptr;
  HRESULT hr = composition->Clone(&range);
  if (SUCCEEDED(hr) && !range) hr = E_UNEXPECTED;
  if (FAILED(hr)) return hr;
  LONG shifted = 0;
  hr = range->Collapse(ec, TF_ANCHOR_START);
  if (SUCCEEDED(hr)) hr = range->ShiftEnd(ec, segment.start + segment.length, &shifted, nullptr);
  if (SUCCEEDED(hr) && shifted != segment.start + segment.length) hr = E_FAIL;
  if (SUCCEEDED(hr)) hr = range->ShiftStart(ec, segment.start, &shifted, nullptr);
  if (SUCCEEDED(hr) && shifted != segment.start) hr = E_FAIL;
  if (SUCCEEDED(hr)) hr = SetAttributeValue(ec, property, range, atom);
  range->Release();
  return hr;
}

}  // namespace

HRESULT ApplyDisplayAttributes(TfEditCookie ec, ITfProperty* property, ITfRange* composition,
                               const std::vector<SegmentAttributeRange>& ranges,
                               const TfGuidAtom (&atoms)[kDisplayAttributeKindCount]) {
  if (!property || !composition) return E_INVALIDARG;
  bool segments_applied = !ranges.empty();
  for (const auto& segment : ranges) {
    if (FAILED(SetSegmentAttribute(ec, property, composition, segment,
                                   atoms[static_cast<size_t>(segment.kind)]))) {
      segments_applied = false;
      break;
    }
  }
  if (segments_applied) return S_OK;
  return SetAttributeValue(ec, property, composition,
                           atoms[static_cast<size_t>(DisplayAttributeKind::Input)]);
}

// --- DisplayAttributeInfo ---

STDMETHODIMP DisplayAttributeInfo::QueryInterface(REFIID riid, void** ppvObj) {
  if (!ppvObj) return E_POINTER;
  *ppvObj = nullptr;
  if (riid == IID_IUnknown || riid == IID_ITfDisplayAttributeInfo) {
    *ppvObj = static_cast<ITfDisplayAttributeInfo*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) DisplayAttributeInfo::AddRef() {
  return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
}
STDMETHODIMP_(ULONG) DisplayAttributeInfo::Release() {
  const auto c = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  if (c == 0) delete this;
  return c;
}

STDMETHODIMP DisplayAttributeInfo::GetGUID(GUID* pguid) {
  if (!pguid) return E_INVALIDARG;
  *pguid = DisplayAttributeGuid(kind_);
  return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::GetDescription(BSTR* pbstrDesc) {
  if (!pbstrDesc) return E_INVALIDARG;
  const wchar_t* description = L"azooKey Input";
  switch (kind_) {
    case DisplayAttributeKind::FocusedSegment:
      description = L"azooKey Focused Segment";
      break;
    case DisplayAttributeKind::ConvertedSegment:
      description = L"azooKey Converted Segment";
      break;
    case DisplayAttributeKind::UnconvertedSegment:
      description = L"azooKey Unconverted Segment";
      break;
    case DisplayAttributeKind::Input:
      break;
  }
  *pbstrDesc = SysAllocString(description);
  return *pbstrDesc ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP DisplayAttributeInfo::GetAttributeInfo(TF_DISPLAYATTRIBUTE* pda) {
  if (!pda) return E_INVALIDARG;
  *pda = DisplayAttributeDefinition(kind_);
  return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::SetAttributeInfo(const TF_DISPLAYATTRIBUTE* /*pda*/) {
  return E_NOTIMPL;
}

STDMETHODIMP DisplayAttributeInfo::Reset() { return S_OK; }

// --- EnumDisplayAttributeInfo ---

STDMETHODIMP EnumDisplayAttributeInfo::QueryInterface(REFIID riid, void** ppvObj) {
  if (!ppvObj) return E_POINTER;
  *ppvObj = nullptr;
  if (riid == IID_IUnknown || riid == IID_IEnumTfDisplayAttributeInfo) {
    *ppvObj = static_cast<IEnumTfDisplayAttributeInfo*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) EnumDisplayAttributeInfo::AddRef() {
  return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
}
STDMETHODIMP_(ULONG) EnumDisplayAttributeInfo::Release() {
  const auto c = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  if (c == 0) delete this;
  return c;
}

STDMETHODIMP EnumDisplayAttributeInfo::Next(ULONG ulCount, ITfDisplayAttributeInfo** rgInfo,
                                            ULONG* pcFetched) {
  if (!rgInfo) return E_INVALIDARG;
  ULONG fetched = 0;
  try {
    // Bound emission by the remaining attribute count rather than `index_ == 0`.
    // The `index_ == 0` guard only ever yields the element at position 0 and only
    // while index_ is exactly 0, so a Skip(n>0) offset (or any future second
    // attribute reached by successive Next calls) would be silently skipped.
    // Gating on kAttributeCount keeps Next and Skip consistent.
    while (fetched < ulCount && index_ < kAttributeCount) {
      auto* info =
          NewComBoundaryObject<DisplayAttributeInfo>(static_cast<DisplayAttributeKind>(index_));
      if (!info) {
        if (pcFetched) *pcFetched = fetched;
        return E_OUTOFMEMORY;
      }
      rgInfo[fetched] = info;
      ++fetched;
      ++index_;
    }
  } catch (const std::bad_alloc&) {
    LogComBoundaryException("EnumDisplayAttributeInfo::Next", E_OUTOFMEMORY);
    if (pcFetched) *pcFetched = fetched;
    return E_OUTOFMEMORY;
  } catch (...) {
    LogComBoundaryException("EnumDisplayAttributeInfo::Next", E_FAIL);
    if (pcFetched) *pcFetched = fetched;
    return E_FAIL;
  }
  if (pcFetched) *pcFetched = fetched;
  return fetched == ulCount ? S_OK : S_FALSE;
}

STDMETHODIMP EnumDisplayAttributeInfo::Skip(ULONG ulCount) {
  index_ += ulCount;
  return S_OK;
}

STDMETHODIMP EnumDisplayAttributeInfo::Reset() {
  index_ = 0;
  return S_OK;
}

STDMETHODIMP EnumDisplayAttributeInfo::Clone(IEnumTfDisplayAttributeInfo** ppEnum) {
  if (!ppEnum) return E_INVALIDARG;
  *ppEnum = nullptr;
  try {
    auto* clone = NewComBoundaryObject<EnumDisplayAttributeInfo>();
    if (!clone) return E_OUTOFMEMORY;
    clone->index_ = index_;
    *ppEnum = clone;
    return S_OK;
  } catch (const std::bad_alloc&) {
    LogComBoundaryException("EnumDisplayAttributeInfo::Clone", E_OUTOFMEMORY);
    return E_OUTOFMEMORY;
  } catch (...) {
    LogComBoundaryException("EnumDisplayAttributeInfo::Clone", E_FAIL);
    return E_FAIL;
  }
}

}  // namespace azookey::tsf
