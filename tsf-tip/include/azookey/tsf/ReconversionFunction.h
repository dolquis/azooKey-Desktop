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

// Where the reconverted range lies on screen, read in the same session as the
// list. `valid` is false when the active view gave no extent (a failure, an
// empty rect or TS_E_NOLAYOUT); `rect` is then unset. `window` is the view's
// window for the physical-point conversion, or null.
struct ReconversionTextExtent {
  bool valid{false};
  RECT rect{};
  HWND window{nullptr};
};

// Receives the list RequestReconversion built, or why there is none, with the
// range's extent. `list` is borrowed: the receiver AddRefs it to keep it.
using ReconversionListCallback =
    std::function<void(HRESULT hr, ITfCandidateList* list, const ReconversionTextExtent& extent)>;

// Receives the outcome of a RequestCandidateReplacement session, inside it:
// further reads use `cookie` rather than request a nested lock.
using CandidateReplacementCallback = std::function<void(HRESULT hr, TfEditCookie cookie)>;
// Asked first inside the session: false when the request went stale.
using CandidateReplacementGuard = std::function<bool()>;

// Replaces `original` in `range` with `replacement` for the TIP's own candidate
// UI (Enter, digit or click). A click arrives outside any application callback,
// where Word refuses a synchronous lock (DEV-1926), so the session is
// TF_ES_ASYNCDONTCARE. Inside it, a false `still_wanted` leaves the text alone
// and `done` gets E_ABORT; a range that no longer reads `original` gets
// TF_E_NOCONVERSION. `done` runs inside the session. Returns S_OK when the
// session ran now and TF_S_ASYNC while it is queued; on failure `done` never runs.
HRESULT RequestCandidateReplacement(ITfRange* range, TfClientId client_id, std::wstring original,
                                    std::wstring replacement,
                                    CandidateReplacementGuard still_wanted,
                                    CandidateReplacementCallback done);

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
