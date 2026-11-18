#pragma once

#include <Windows.h>
#include <msctf.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace azookey::tsf {

inline constexpr GUID kCandidateListUiElementGuid = {
    0xb3b0d31b, 0xae6a, 0x4f7c, {0x8d, 0x38, 0x5c, 0xfa, 0x8d, 0x6e, 0xe7, 0x24}};

class CandidateListUIElement final : public ITfCandidateListUIElement {
 public:
  CandidateListUIElement(std::vector<std::wstring> items, int selected_idx,
                         std::wstring notice = {});

  CandidateListUIElement(const CandidateListUIElement&) = delete;
  CandidateListUIElement& operator=(const CandidateListUIElement&) = delete;

  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  STDMETHODIMP GetDescription(BSTR* pbstrDescription) override;
  STDMETHODIMP GetGUID(GUID* pguid) override;
  STDMETHODIMP Show(BOOL bShow) override;
  STDMETHODIMP IsShown(BOOL* pbShow) override;

  STDMETHODIMP GetUpdatedFlags(DWORD* pdwFlags) override;
  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr** ppdim) override;
  STDMETHODIMP GetCount(UINT* puCount) override;
  STDMETHODIMP GetSelection(UINT* puIndex) override;
  STDMETHODIMP GetString(UINT uIndex, BSTR* pstr) override;
  STDMETHODIMP GetPageIndex(UINT* pIndex, UINT uSize, UINT* puPageCnt) override;
  STDMETHODIMP SetPageIndex(UINT* pIndex, UINT uPageCnt) override;
  STDMETHODIMP GetCurrentPage(UINT* puPage) override;

  // Replacing the items with different ones drops the split an app set with
  // SetPageIndex. Both add TF_CLUIE_PAGEINDEX and TF_CLUIE_CURRENTPAGE to the
  // flags when the split or the current page changes (tsf-deep-integration-spec
  // §2.3).
  void Update(std::vector<std::wstring> items, int selected_idx, DWORD updated_flags);
  void SetSelection(int selected_idx, DWORD updated_flags);
  void SetShown(bool shown);
  void SetShowCallback(std::function<void(bool)> callback);
  // The split an app set with SetPageIndex, while the app draws the list;
  // empty (nine per page, core/include/azookey/core/CandidatePage.h) while the
  // TIP draws it or no split was set.
  const std::vector<uint32_t>& AppPageSplit() const;

 private:
  static int ClampSelection(int selected_idx, size_t count);
  // The page holding the selection under AppPageSplit.
  size_t CurrentPage() const;

  LONG ref_count_{1};
  std::vector<std::wstring> items_;
  std::wstring notice_;
  int selected_idx_{-1};
  DWORD updated_flags_{TF_CLUIE_COUNT | TF_CLUIE_STRING | TF_CLUIE_SELECTION};
  bool shown_{false};
  std::function<void(bool)> show_callback_;
  // Validated against items_ by SetPageIndex; empty when none was set.
  std::vector<uint32_t> app_page_starts_;
};

}  // namespace azookey::tsf
