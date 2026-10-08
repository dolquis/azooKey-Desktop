#pragma once

#include <Windows.h>
#include <msctf.h>

#include <cstddef>
#include <vector>

namespace azookey::tsf {

#ifdef AZOOKEY_TSF_TESTING
namespace testing {
void FailNextComBoundaryAllocationForTest();
void ClearComBoundaryAllocationFailureForTest();
bool ConsumeComBoundaryAllocationFailureForTest();
}  // namespace testing
#endif

// GUID for the input-composition underline display attribute registered by the TIP.
// It covers the whole composition whenever per-segment attributes do not apply.
// {5D8F0A63-2B5E-4F8C-A1D4-7E9B2C3F4A5D}
inline constexpr GUID kInputAttributeGuid = {0x5d8f0a63,
                                              0x2b5e,
                                              0x4f8c,
                                              {0xa1, 0xd4, 0x7e, 0x9b, 0x2c, 0x3f, 0x4a, 0x5d}};

// Per-segment attributes for batch conversion (tsf-deep-integration-spec §5).
// {06C5C71C-F8B6-4879-B26A-01ACD34DEEB8}
inline constexpr GUID kFocusedSegmentAttributeGuid = {
    0x06c5c71c, 0xf8b6, 0x4879, {0xb2, 0x6a, 0x01, 0xac, 0xd3, 0x4d, 0xee, 0xb8}};
// {74EBD680-D599-4066-9142-52EBC51EC1CE}
inline constexpr GUID kConvertedSegmentAttributeGuid = {
    0x74ebd680, 0xd599, 0x4066, {0x91, 0x42, 0x52, 0xeb, 0xc5, 0x1e, 0xc1, 0xce}};
// {A46C0379-4629-4931-A1F8-82D472CE18F7}
inline constexpr GUID kUnconvertedSegmentAttributeGuid = {
    0xa46c0379, 0x4629, 0x4931, {0xa1, 0xf8, 0x82, 0xd4, 0x72, 0xce, 0x18, 0xf7}};

// The attributes the TIP provides, in enumeration order.
enum class DisplayAttributeKind : unsigned char {
  Input,
  FocusedSegment,
  ConvertedSegment,
  UnconvertedSegment
};
inline constexpr ULONG kDisplayAttributeKindCount = 4;

const GUID& DisplayAttributeGuid(DisplayAttributeKind kind);
// Colors are left to the application (TF_CT_NONE); segments differ by bAttr and
// the underline style alone (spec §5.6).
TF_DISPLAYATTRIBUTE DisplayAttributeDefinition(DisplayAttributeKind kind);
bool FindDisplayAttributeKind(REFGUID guid, DisplayAttributeKind* kind);

// One batch-conversion segment as it appears in the preedit.
struct DisplayedSegment {
  LONG length;     // UTF-16 code units.
  bool converted;  // Shows a candidate rather than its reading.
};

struct SegmentAttributeRange {
  LONG start;  // UTF-16 offset from the start of the composition.
  LONG length;
  DisplayAttributeKind kind;
};

// Attribute ranges for the segments: the focused one, then converted or
// unconverted by what each shows. Empty segments get no range.
std::vector<SegmentAttributeRange> BuildSegmentAttributeRanges(
    const std::vector<DisplayedSegment>& segments, size_t focused_index);

// Segment under a mouse event reported by ITfMouseSink::OnMouseEvent, where
// uEdge counts characters from the composition start and quadrants 0 and 1
// precede the edge. Returns false when there are no non-empty segments.
bool SegmentIndexAtMouseEdge(const std::vector<DisplayedSegment>& segments, ULONG edge,
                             ULONG quadrant, size_t* index);

// Sets GUID_PROP_ATTRIBUTE over the composition: each range with its segment
// attribute, or the input attribute over the whole composition when there are
// no ranges, an atom is missing, or any segment fails (spec §5.6).
// atoms is indexed by DisplayAttributeKind.
HRESULT ApplyDisplayAttributes(TfEditCookie ec, ITfProperty* property, ITfRange* composition,
                               const std::vector<SegmentAttributeRange>& ranges,
                               const TfGuidAtom (&atoms)[kDisplayAttributeKindCount]);

// ITfDisplayAttributeInfo for one of the attributes above.
class DisplayAttributeInfo final : public ITfDisplayAttributeInfo {
 public:
  explicit DisplayAttributeInfo(DisplayAttributeKind kind = DisplayAttributeKind::Input)
      : kind_(kind) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  STDMETHODIMP GetGUID(GUID* pguid) override;
  STDMETHODIMP GetDescription(BSTR* pbstrDesc) override;
  STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* pda) override;
  STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE* pda) override;
  STDMETHODIMP Reset() override;

 private:
  DisplayAttributeKind kind_;
  LONG ref_count_{1};
};

// IEnumTfDisplayAttributeInfo enumerating every DisplayAttributeKind.
class EnumDisplayAttributeInfo final : public IEnumTfDisplayAttributeInfo {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  STDMETHODIMP Next(ULONG ulCount, ITfDisplayAttributeInfo** rgInfo, ULONG* pcFetched) override;
  STDMETHODIMP Skip(ULONG ulCount) override;
  STDMETHODIMP Reset() override;
  STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo** ppEnum) override;

 private:
  static constexpr ULONG kAttributeCount = kDisplayAttributeKindCount;

  LONG ref_count_{1};
  ULONG index_{0};
};

}  // namespace azookey::tsf
