#pragma once

#include <Windows.h>
#include <msctf.h>

#include <functional>
#include <string>
#include <vector>

namespace azookey::tsf {

inline constexpr GUID kCandidateListUiElementGuid = {
    0xb3b0d31b, 0xae6a, 0x4f7c, {0x8d, 0x38, 0x5c, 0xfa, 0x8d, 0x6e, 0xe7, 0x24}};

class CandidateListUIElement final : public ITfCandidateListUIElement {
 public:
  CandidateListUIElement(std::vector<std::wstring> items, int selected_idx);

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

  void Update(std::vector<std::wstring> items, int selected_idx, DWORD updated_flags);
  void SetSelection(int selected_idx, DWORD updated_flags);
  void SetShown(bool shown);
  void SetShowCallback(std::function<void(bool)> callback);

 private:
  static int ClampSelection(int selected_idx, size_t count);

  LONG ref_count_{1};
  std::vector<std::wstring> items_;
  int selected_idx_{-1};
  DWORD updated_flags_{TF_CLUIE_COUNT | TF_CLUIE_STRING | TF_CLUIE_SELECTION};
  bool shown_{false};
  std::function<void(bool)> show_callback_;
};

}  // namespace azookey::tsf
