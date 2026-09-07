#include "azookey/tsf/BracketEditSession.h"

#include <wrl/client.h>

#include <array>
#include <functional>
#include <new>
#include <utility>

#include "azookey/tsf/TextService.h"

namespace azookey::tsf {
namespace {
using Microsoft::WRL::ComPtr;
using ActionType = core::BracketPairingActionType;

class SynchronousSession final : public ITfEditSession {
 public:
  explicit SynchronousSession(std::function<HRESULT(TfEditCookie)> body) : body_(std::move(body)) {}
  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfEditSession) return E_NOINTERFACE;
    *object = static_cast<ITfEditSession*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&references_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = InterlockedDecrement(&references_);
    if (!remaining) delete this;
    return remaining;
  }
  STDMETHODIMP DoEditSession(TfEditCookie cookie) override {
    if (!body_ || executed) return E_UNEXPECTED;
    executed = true;
    try {
      return body_(cookie);
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }
  void Disarm() { body_ = {}; }
  bool executed{false};

 private:
  LONG references_{1};
  std::function<HRESULT(TfEditCookie)> body_;
};

HRESULT RunSync(ITfContext* context, TfClientId client_id, DWORD access,
                std::function<HRESULT(TfEditCookie)> body) {
  if (!context) return E_INVALIDARG;
  ComPtr<SynchronousSession> session;
  session.Attach(new SynchronousSession(std::move(body)));
  HRESULT result = E_FAIL;
  const HRESULT request =
      context->RequestEditSession(client_id, session.Get(), TF_ES_SYNC | access, &result);
  // Even a provider that erroneously retains a sync request cannot execute its
  // stack captures later. Success also requires that the callback actually ran.
  session->Disarm();
  if (FAILED(request)) return request;
  return session->executed ? result : E_FAIL;
}

HRESULT Selection(ITfContext* context, TfEditCookie cookie, ComPtr<ITfRange>& range) {
  TF_SELECTION selection{};
  ULONG fetched = 0;
  const HRESULT hr = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
  range.Attach(selection.range);
  if (FAILED(hr)) return hr;
  return fetched == 1 && range ? S_OK : E_FAIL;
}

core::EditContextHint ReadAtCookie(ITfContext* context, TfEditCookie cookie) {
  core::EditContextHint hint;
  ComPtr<ITfRange> selection;
  if (FAILED(Selection(context, cookie, selection))) return {};
  BOOL empty = FALSE;
  if (FAILED(selection->IsEmpty(cookie, &empty))) return {};
  hint.selection_collapsed = empty != FALSE;
  if (!empty) return hint;
  for (const bool before : {true, false}) {
    ComPtr<ITfRange> adjacent;
    if (FAILED(selection->Clone(&adjacent)) || !adjacent) return {};
    if (FAILED(adjacent->Collapse(cookie, before ? TF_ANCHOR_START : TF_ANCHOR_END))) return {};
    LONG shifted = 0;
    const HRESULT hr = before ? adjacent->ShiftStart(cookie, -1, &shifted, nullptr)
                              : adjacent->ShiftEnd(cookie, 1, &shifted, nullptr);
    if (FAILED(hr)) return {};
    if (shifted == 0) continue;
    if (shifted != (before ? -1 : 1)) return {};
    WCHAR character{};
    ULONG count = 0;
    if (FAILED(adjacent->GetText(cookie, 0, &character, 1, &count)) || count != 1) return {};
    (before ? hint.char_before : hint.char_after) = character;
  }
  return hint;
}

HRESULT PlaceCaret(ITfContext* context, TfEditCookie cookie, ITfRange* range, bool inside) {
  ComPtr<ITfRange> caret;
  HRESULT hr = range->Clone(&caret);
  if (FAILED(hr) || !caret) return FAILED(hr) ? hr : E_FAIL;
  range = caret.Get();
  hr = range->Collapse(cookie, TF_ANCHOR_END);
  if (FAILED(hr)) return hr;
  if (inside) {
    LONG shifted = 0;
    hr = range->ShiftStart(cookie, -1, &shifted, nullptr);
    if (FAILED(hr) || shifted != -1) return FAILED(hr) ? hr : E_FAIL;
    hr = range->Collapse(cookie, TF_ANCHOR_START);
    if (FAILED(hr)) return hr;
  }
  TF_SELECTION selection{};
  selection.range = range;
  selection.style.ase = TF_AE_NONE;
  return context->SetSelection(cookie, 1, &selection);
}
}  // namespace

core::EditContextHint BracketEditSession::ReadHint(ITfContext* context, TfClientId client_id) {
  core::EditContextHint hint;
  const HRESULT hr = RunSync(context, client_id, TF_ES_READ, [&](TfEditCookie cookie) {
    hint = ReadAtCookie(context, cookie);
    return S_OK;
  });
  return SUCCEEDED(hr) ? hint : core::EditContextHint{};
}

HRESULT BracketEditSession::Apply(TextService& service, ITfContext* context, TfClientId client_id,
                                  core::BracketPairingAction action,
                                  const core::BracketSettings& settings, bool& applied) {
  applied = false;
  return RunSync(context, client_id, TF_ES_READWRITE, [&](TfEditCookie cookie) {
    // Revalidate under the write lock: OnTestKeyDown and even the preceding
    // read session do not authorize deleting text that has since changed.
    const auto fresh = ReadAtCookie(context, cookie);
    if (action.type == ActionType::kDeletePair &&
        (fresh.selection_collapsed != true || fresh.char_before != action.open ||
         fresh.char_after != action.close))
      return S_FALSE;
    if (action.type == ActionType::kSkipClosing &&
        (fresh.selection_collapsed != true || fresh.char_after != action.close)) {
      action.type = ActionType::kInsertLiteral;
      action.open = action.close;
    }
    if (action.type == ActionType::kInsertPair && fresh.selection_collapsed != true) {
      action.type = ActionType::kInsertLiteral;
    }
    if (action.type == ActionType::kInsertPair && action.open == action.close) {
      action =
          core::EvaluateBracketInput(action.open, false, fresh, settings.pairing, settings.Table());
    }
    if (action.type == ActionType::kWrapSelection && fresh.selection_collapsed != false)
      return S_FALSE;
    ComPtr<ITfRange> range;
    HRESULT hr = Selection(context, cookie, range);
    if (FAILED(hr)) return hr;
    if (action.type == ActionType::kWrapSelection) {
      ComPtr<ITfRange> reader;
      hr = range->Clone(&reader);
      if (FAILED(hr) || !reader) return FAILED(hr) ? hr : E_FAIL;
      std::wstring text(1, static_cast<WCHAR>(action.open));
      std::array<WCHAR, 4096> buffer{};
      for (;;) {
        BOOL empty = FALSE;
        hr = reader->IsEmpty(cookie, &empty);
        if (FAILED(hr)) return hr;
        if (empty) break;
        ULONG count = 0;
        hr = reader->GetText(cookie, TF_TF_MOVESTART, buffer.data(),
                             static_cast<ULONG>(buffer.size()), &count);
        if (FAILED(hr)) return hr;
        if (!count || count > buffer.size() || text.size() + count > 65537) return S_FALSE;
        text.append(buffer.data(), count);
      }
      text.push_back(static_cast<WCHAR>(action.close));
      hr = range->SetText(cookie, 0, text.data(), static_cast<LONG>(text.size()));
      if (FAILED(hr)) return hr;
      applied = true;
      return PlaceCaret(context, cookie, range.Get(), false);
    }
    LONG shifted = 0;
    if (action.type == ActionType::kSkipClosing) {
      hr = range->ShiftEnd(cookie, 1, &shifted, nullptr);
      if (FAILED(hr) || shifted != 1) return FAILED(hr) ? hr : E_FAIL;
      hr = PlaceCaret(context, cookie, range.Get(), false);
      applied = SUCCEEDED(hr);
      return hr;
    }
    if (action.type == ActionType::kDeletePair) {
      hr = range->ShiftStart(cookie, -1, &shifted, nullptr);
      if (FAILED(hr) || shifted != -1) return FAILED(hr) ? hr : E_FAIL;
      hr = range->ShiftEnd(cookie, 1, &shifted, nullptr);
      if (FAILED(hr) || shifted != 1) return FAILED(hr) ? hr : E_FAIL;
      hr = range->SetText(cookie, 0, L"", 0);
      if (FAILED(hr)) return hr;
      applied = true;
      return PlaceCaret(context, cookie, range.Get(), false);
    }
    const bool pair = action.type == ActionType::kInsertPair;
    if (!pair && action.type != ActionType::kInsertLiteral) return S_FALSE;
    const WCHAR text[]{static_cast<WCHAR>(action.open), static_cast<WCHAR>(action.close)};
    ComPtr<ITfComposition> composition;
    if (pair && settings.trigger == core::BracketPairingTrigger::Composition) {
      ComPtr<ITfContextComposition> context_composition;
      hr = context->QueryInterface(IID_PPV_ARGS(&context_composition));
      if (FAILED(hr)) return hr;
      hr = context_composition->StartComposition(cookie, range.Get(), &service, &composition);
      if (FAILED(hr) || !composition) return FAILED(hr) ? hr : E_FAIL;
      hr = composition->GetRange(&range);
      if (FAILED(hr) || !range) {
        composition->EndComposition(cookie);
        return FAILED(hr) ? hr : E_FAIL;
      }
    }
    hr = range->SetText(cookie, 0, text, pair ? 2 : 1);
    if (FAILED(hr)) {
      if (composition) composition->EndComposition(cookie);
      return hr;
    }
    applied = true;  // A subsequent caret failure must not replay this insertion.
    if (composition) {
      service.composition_ = composition.Detach();
      service.bracket_composition_ = true;
    }
    return PlaceCaret(context, cookie, range.Get(), pair);
  });
}

HRESULT BracketEditSession::Finish(TextService& service, ITfContext* context, TfClientId client_id,
                                   bool cancel) {
  if (!service.bracket_composition_ || !service.composition_) return S_OK;
  // A lifecycle cleanup may have been refused before focus moved. Finish the
  // composition under its owning context, never using a new document's cookie.
  ComPtr<ITfContext> owner = service.active_context_ ? service.active_context_ : context;
  context = owner.Get();
  return RunSync(context, client_id, TF_ES_READWRITE, [&](TfEditCookie cookie) {
    ComPtr<ITfComposition> composition = service.composition_;
    ComPtr<ITfRange> range;
    HRESULT hr = composition->GetRange(&range);
    if (FAILED(hr) || !range) return FAILED(hr) ? hr : E_FAIL;
    if (cancel) {
      hr = range->SetText(cookie, 0, L"", 0);
      if (FAILED(hr)) return hr;
    }
    hr = composition->EndComposition(cookie);
    if (FAILED(hr)) return hr;
    if (service.composition_ == composition.Get()) {
      service.composition_->Release();
      service.composition_ = nullptr;
    }
    service.bracket_composition_ = false;
    // The text is already finalized. A caret failure cannot be retried as a
    // commit; report it to the caller while leaving no pending text to replay.
    return PlaceCaret(context, cookie, range.Get(), !cancel);
  });
}
}  // namespace azookey::tsf
