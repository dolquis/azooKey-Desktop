#include "azookey/tsf/CandidateListUIElement.h"

#include <OleAuto.h>

#include <algorithm>
#include <utility>

namespace azookey::tsf {

CandidateListUIElement::CandidateListUIElement(std::vector<std::wstring> items, int selected_idx)
    : items_(std::move(items)), selected_idx_(ClampSelection(selected_idx, items_.size())) {}

STDMETHODIMP CandidateListUIElement::QueryInterface(REFIID riid, void** ppvObj) {
  if (!ppvObj) return E_POINTER;
  *ppvObj = nullptr;
  if (riid == IID_IUnknown || riid == IID_ITfUIElement || riid == IID_ITfCandidateListUIElement) {
    *ppvObj = static_cast<ITfCandidateListUIElement*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) CandidateListUIElement::AddRef() {
  return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
}

STDMETHODIMP_(ULONG) CandidateListUIElement::Release() {
  const auto c = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  if (c == 0) delete this;
  return c;
}

STDMETHODIMP CandidateListUIElement::GetDescription(BSTR* pbstrDescription) {
  if (!pbstrDescription) return E_INVALIDARG;
  *pbstrDescription = SysAllocString(L"azooKey Candidate List");
  return *pbstrDescription ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CandidateListUIElement::GetGUID(GUID* pguid) {
  if (!pguid) return E_INVALIDARG;
  *pguid = kCandidateListUiElementGuid;
  return S_OK;
}

STDMETHODIMP CandidateListUIElement::Show(BOOL bShow) {
  shown_ = bShow != FALSE;
  if (show_callback_) show_callback_(shown_);
  return S_OK;
}

STDMETHODIMP CandidateListUIElement::IsShown(BOOL* pbShow) {
  if (!pbShow) return E_INVALIDARG;
  *pbShow = shown_ ? TRUE : FALSE;
  return S_OK;
}

STDMETHODIMP CandidateListUIElement::GetUpdatedFlags(DWORD* pdwFlags) {
  if (!pdwFlags) return E_INVALIDARG;
  *pdwFlags = updated_flags_;
  return S_OK;
}

STDMETHODIMP CandidateListUIElement::GetDocumentMgr(ITfDocumentMgr** ppdim) {
  if (!ppdim) return E_INVALIDARG;
  *ppdim = nullptr;
  return S_FALSE;
}

STDMETHODIMP CandidateListUIElement::GetCount(UINT* puCount) {
  if (!puCount) return E_INVALIDARG;
  *puCount = static_cast<UINT>(items_.size());
  return S_OK;
}

STDMETHODIMP CandidateListUIElement::GetSelection(UINT* puIndex) {
  if (!puIndex) return E_INVALIDARG;
  if (selected_idx_ < 0) return S_FALSE;
  *puIndex = static_cast<UINT>(selected_idx_);
  return S_OK;
}

STDMETHODIMP CandidateListUIElement::GetString(UINT uIndex, BSTR* pstr) {
  if (!pstr) return E_INVALIDARG;
  *pstr = nullptr;
  if (uIndex >= items_.size()) return E_INVALIDARG;
  const std::wstring& item = items_[uIndex];
  if (item.empty()) {
    *pstr = SysAllocString(L"");
    return *pstr ? S_OK : E_OUTOFMEMORY;
  }
  *pstr = SysAllocStringLen(item.data(), static_cast<UINT>(item.size()));
  return *pstr ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CandidateListUIElement::GetPageIndex(UINT* pIndex, UINT uSize, UINT* puPageCnt) {
  if (!puPageCnt) return E_INVALIDARG;
  *puPageCnt = 1;
  if (uSize == 0) return S_OK;
  if (!pIndex) return E_INVALIDARG;
  pIndex[0] = 0;
  return S_OK;
}

STDMETHODIMP CandidateListUIElement::SetPageIndex(UINT* pIndex, UINT uPageCnt) {
  UNREFERENCED_PARAMETER(pIndex);
  return uPageCnt <= 1 ? S_OK : E_INVALIDARG;
}

STDMETHODIMP CandidateListUIElement::GetCurrentPage(UINT* puPage) {
  if (!puPage) return E_INVALIDARG;
  *puPage = 0;
  return S_OK;
}

void CandidateListUIElement::Update(std::vector<std::wstring> items, int selected_idx,
                                    DWORD updated_flags) {
  items_ = std::move(items);
  selected_idx_ = ClampSelection(selected_idx, items_.size());
  updated_flags_ = updated_flags;
}

void CandidateListUIElement::SetSelection(int selected_idx, DWORD updated_flags) {
  selected_idx_ = ClampSelection(selected_idx, items_.size());
  updated_flags_ = updated_flags;
}

void CandidateListUIElement::SetShown(bool shown) { shown_ = shown; }

void CandidateListUIElement::SetShowCallback(std::function<void(bool)> callback) {
  show_callback_ = std::move(callback);
}

int CandidateListUIElement::ClampSelection(int selected_idx, size_t count) {
  if (count == 0) return -1;
  return std::clamp(selected_idx, 0, static_cast<int>(count) - 1);
}

}  // namespace azookey::tsf
