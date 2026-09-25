#pragma once

#include <Windows.h>
#include <ctffunc.h>
#include <msctf.h>

#include <functional>
#include <string>
#include <vector>

namespace azookey::tsf {

// The provider supplies candidates from the Host. It must impose a bounded
// deadline: TSF calls GetReconversion on the caller's apartment.
using ReconversionCandidateProvider =
    std::function<HRESULT(ITfRange* range, const std::wstring& surface,
                          std::vector<std::wstring>& candidates)>;

class ReconversionFunction final : public ITfFnReconversion {
 public:
  ReconversionFunction(TfClientId client_id, ReconversionCandidateProvider provider);
  ReconversionFunction(const ReconversionFunction&) = delete;
  ReconversionFunction& operator=(const ReconversionFunction&) = delete;

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;
  STDMETHODIMP GetDisplayName(BSTR* name) override;
  STDMETHODIMP QueryRange(ITfRange* range, ITfRange** new_range, BOOL* convertible) override;
  STDMETHODIMP GetReconversion(ITfRange* range, ITfCandidateList** candidates) override;
  STDMETHODIMP Reconvert(ITfRange* range) override;

  // Convenience entry point for the conversion key; call outside an EditSession.
  HRESULT ReconvertSelection(ITfContext* context);
  HRESULT CaptureSelection(ITfContext* context, ITfRange** range, std::wstring& surface);

 private:
  ~ReconversionFunction() = default;

  LONG ref_count_{1};
  TfClientId client_id_;
  ReconversionCandidateProvider provider_;
};

}  // namespace azookey::tsf
