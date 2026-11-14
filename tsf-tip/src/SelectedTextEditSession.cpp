#include "azookey/tsf/SelectedTextEditSession.h"

#include <array>
#include <memory>
#include <new>
#include <utility>

#include "azookey/tsf/TipRuntimeLog.h"

namespace azookey::tsf {
namespace {

// The edit session the helpers below run their work in. The action runs on
// the TSF owner thread with the cookie the application granted.
class ActionEditSession final : public ITfEditSession {
 public:
  explicit ActionEditSession(std::function<HRESULT(TfEditCookie)> action)
      : action_(std::move(action)) {}
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfEditSession) return E_NOINTERFACE;
    *out = static_cast<ITfEditSession*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG refs = --refs_;
    if (refs == 0) delete this;
    return refs;
  }
  STDMETHODIMP DoEditSession(TfEditCookie cookie) override {
    try {
      return action_(cookie);
    } catch (const std::bad_alloc&) {
      LogComBoundaryException("SelectedTextEditSession::DoEditSession", E_OUTOFMEMORY);
      return E_OUTOFMEMORY;
    } catch (...) {
      LogComBoundaryException("SelectedTextEditSession::DoEditSession", E_FAIL);
      return E_FAIL;
    }
  }

 private:
  ULONG refs_{1};
  std::function<HRESULT(TfEditCookie)> action_;
};

// Reads `range` into `out`, stopping once more than `max_units` code units
// would be needed: S_FALSE then means "longer than max_units".
HRESULT ReadRange(ITfRange* range, TfEditCookie cookie, size_t max_units, std::wstring& out) {
  out.clear();
  Microsoft::WRL::ComPtr<ITfRange> reader;
  HRESULT hr = range->Clone(&reader);
  if (FAILED(hr) || !reader) return FAILED(hr) ? hr : E_FAIL;
  std::array<wchar_t, 256> buffer{};
  while (true) {
    BOOL empty = FALSE;
    hr = reader->IsEmpty(cookie, &empty);
    if (FAILED(hr)) return hr;
    if (empty) return S_OK;
    if (out.size() > max_units) return S_FALSE;
    ULONG length = 0;
    hr = reader->GetText(cookie, TF_TF_MOVESTART, buffer.data(), static_cast<ULONG>(buffer.size()),
                         &length);
    if (FAILED(hr)) return hr;
    if (length == 0 || length > buffer.size()) return E_FAIL;
    out.append(buffer.data(), length);
  }
}

}  // namespace

SelectedTextRead ReadSelectedText(ITfContext* context, TfClientId client_id, size_t max_units,
                                  SelectedTextCapture& capture) noexcept {
  if (!context) return SelectedTextRead::Failed;
  try {
    SelectedTextRead outcome = SelectedTextRead::Failed;
    Microsoft::WRL::ComPtr<ITfRange> selected;
    std::wstring text;
    auto* session = new (std::nothrow) ActionEditSession([&](TfEditCookie cookie) -> HRESULT {
      TF_SELECTION selection{};
      ULONG fetched = 0;
      HRESULT hr = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
      if (FAILED(hr)) return hr;
      if (fetched != 1 || !selection.range) {
        outcome = SelectedTextRead::Empty;
        return S_OK;
      }
      selected.Attach(selection.range);
      BOOL empty = FALSE;
      hr = selected->IsEmpty(cookie, &empty);
      if (FAILED(hr)) return hr;
      if (empty) {
        outcome = SelectedTextRead::Empty;
        return S_OK;
      }
      hr = ReadRange(selected.Get(), cookie, max_units, text);
      if (FAILED(hr)) return hr;
      outcome = hr == S_FALSE || text.size() > max_units ? SelectedTextRead::TooLong
                : text.empty()                           ? SelectedTextRead::Empty
                                                         : SelectedTextRead::Ok;
      return S_OK;
    });
    if (!session) return SelectedTextRead::Failed;
    HRESULT session_hr = E_FAIL;
    const HRESULT hr =
        context->RequestEditSession(client_id, session, TF_ES_SYNC | TF_ES_READ, &session_hr);
    session->Release();
    if (FAILED(hr) || session_hr != S_OK) return SelectedTextRead::Failed;
    if (outcome == SelectedTextRead::Ok) {
      capture.range = std::move(selected);
      capture.text = std::move(text);
    }
    return outcome;
  } catch (...) {
    return SelectedTextRead::Failed;
  }
}

HRESULT RequestSelectedTextReplacement(ITfRange* range, TfClientId client_id, std::wstring original,
                                       std::wstring replacement,
                                       std::function<void(HRESULT)> done) noexcept {
  if (!range || !done) return E_INVALIDARG;
  try {
    Microsoft::WRL::ComPtr<ITfContext> context;
    HRESULT hr = range->GetContext(&context);
    if (FAILED(hr) || !context) return FAILED(hr) ? hr : E_FAIL;
    // A queued session outlives this call, so it owns what it uses.
    auto ran = std::make_shared<bool>(false);
    Microsoft::WRL::ComPtr<ITfRange> target(range);
    auto* session = new (std::nothrow) ActionEditSession(
        [target, ran, original = std::move(original), replacement = std::move(replacement),
         done = std::move(done)](TfEditCookie cookie) -> HRESULT {
          *ran = true;
          std::wstring current;
          HRESULT result = ReadRange(target.Get(), cookie, original.size(), current);
          if (SUCCEEDED(result)) {
            if (result == S_FALSE || current != original) {
              result = TF_E_NOCONVERSION;
            } else {
              result = target->SetText(cookie, 0, replacement.data(),
                                       static_cast<LONG>(replacement.size()));
            }
          }
          done(result);
          return S_OK;
        });
    if (!session) return E_OUTOFMEMORY;
    HRESULT session_hr = E_FAIL;
    hr = context->RequestEditSession(client_id, session, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                                     &session_hr);
    session->Release();
    if (*ran) return S_OK;
    if (SUCCEEDED(hr) && session_hr == TF_S_ASYNC) return TF_S_ASYNC;
    return FAILED(hr) ? hr : (FAILED(session_hr) ? session_hr : E_FAIL);
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

}  // namespace azookey::tsf
