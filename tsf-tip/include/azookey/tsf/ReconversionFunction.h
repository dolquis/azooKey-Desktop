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
using ReconversionCandidateProvider = std::function<HRESULT(
    ITfRange* range, const std::wstring& surface, std::vector<std::wstring>& candidates)>;

// Receives the list RequestReconversion built, or why there is none. `list` is
// borrowed: the receiver AddRefs it to keep it.
using ReconversionListCallback = std::function<void(HRESULT hr, ITfCandidateList* list)>;

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
  HRESULT CaptureSelection(ITfContext* context, ITfRange** range, std::wstring& surface,
                           ITfRange* expected_range = nullptr);
  // GetReconversion for callers outside an application callback, where an
  // application may refuse a synchronous lock (Word: TS_E_SYNCHRONOUS). The
  // range is read in a TF_ES_ASYNCDONTCARE session and the provider and `done`
  // run inside it. Returns TF_S_ASYNC while the session is queued; on failure
  // `done` never runs.
  HRESULT RequestReconversion(ITfRange* range, ReconversionListCallback done);

 private:
  ~ReconversionFunction() = default;
  HRESULT BuildCandidateList(ITfRange* range, std::wstring surface, ITfCandidateList** candidates,
                             const char* stage);

  LONG ref_count_{1};
  TfClientId client_id_;
  ReconversionCandidateProvider provider_;
};

}  // namespace azookey::tsf
