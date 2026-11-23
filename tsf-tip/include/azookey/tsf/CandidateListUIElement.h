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

// The ITfCandidateListUIElementBehavior call an app that draws the list makes.
enum class CandidateListBehavior { SetSelection, Finalize, Abort };

// `index` is the candidate SetSelection names, below the list's count; 0 for
// Finalize and Abort.
using CandidateListBehaviorHandler = HRESULT (*)(CandidateListBehavior behavior, UINT index,
                                                 void* context) noexcept;

class CandidateListUIElement final : public ITfCandidateListUIElementBehavior {
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

  // tsf-deep-integration-spec §2.3: each goes to the handler, which decides on
  // the TIP's selection; the element's own moves only when the TIP moves it.
  // An index past the list is E_INVALIDARG, also once the list has ended;
  // any other call without a handler (the list has ended) is E_FAIL.
  STDMETHODIMP SetSelection(UINT nIndex) override;
  STDMETHODIMP Finalize() override;
  STDMETHODIMP Abort() override;

  // Replacing the items with different ones drops the split an app set with
  // SetPageIndex. Both add TF_CLUIE_PAGEINDEX and TF_CLUIE_CURRENTPAGE to the
  // flags when the split or the current page changes (tsf-deep-integration-spec
  // §2.3).
  void Update(std::vector<std::wstring> items, int selected_idx, DWORD updated_flags);
  void UpdateSelection(int selected_idx, DWORD updated_flags);
  void SetShown(bool shown);
  void SetShowCallback(std::function<void(bool)> callback);
  // A null handler detaches the element: an app may keep it after the list ends.
  void SetBehaviorHandler(CandidateListBehaviorHandler handler, void* context);
  // The split an app set with SetPageIndex, while the app draws the list;
  // empty (nine per page, core/include/azookey/core/CandidatePage.h) while the
  // TIP draws it or no split was set.
  const std::vector<uint32_t>& AppPageSplit() const;

 private:
  static int ClampSelection(int selected_idx, size_t count);
  // The page holding the selection under AppPageSplit.
  size_t CurrentPage() const;
  HRESULT CallBehaviorHandler(CandidateListBehavior behavior, UINT index);

  LONG ref_count_{1};
  std::vector<std::wstring> items_;
  std::wstring notice_;
  int selected_idx_{-1};
  DWORD updated_flags_{TF_CLUIE_COUNT | TF_CLUIE_STRING | TF_CLUIE_SELECTION};
  bool shown_{false};
  std::function<void(bool)> show_callback_;
  CandidateListBehaviorHandler behavior_handler_{nullptr};
  void* behavior_context_{nullptr};
  // Validated against items_ by SetPageIndex; empty when none was set.
  std::vector<uint32_t> app_page_starts_;
};

}  // namespace azookey::tsf
