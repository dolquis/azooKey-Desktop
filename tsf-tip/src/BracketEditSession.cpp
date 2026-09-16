#include "azookey/tsf/BracketEditSession.h"

#include <wrl/client.h>

#include <array>
#include <functional>
#include <new>
#include <optional>
#include <string>
#include <string_view>
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

// Reads the single code unit next to `range` on the given side. `readable`
// separates a document edge, which legitimately has no character, from a
// provider that refused the read.
std::optional<WCHAR> ReadNeighbour(TfEditCookie cookie, ITfRange* range, bool before,
                                   bool& readable) {
  readable = false;
  ComPtr<ITfRange> adjacent;
  if (FAILED(range->Clone(&adjacent)) || !adjacent) return std::nullopt;
  if (FAILED(adjacent->Collapse(cookie, before ? TF_ANCHOR_START : TF_ANCHOR_END)))
    return std::nullopt;
  LONG shifted = 0;
  const HRESULT hr = before ? adjacent->ShiftStart(cookie, -1, &shifted, nullptr)
                            : adjacent->ShiftEnd(cookie, 1, &shifted, nullptr);
  if (FAILED(hr)) return std::nullopt;
  if (shifted == 0) {
    readable = true;
    return std::nullopt;
  }
  if (shifted != (before ? -1 : 1)) return std::nullopt;
  WCHAR character{};
  ULONG count = 0;
  if (FAILED(adjacent->GetText(cookie, 0, &character, 1, &count)) || count != 1)
    return std::nullopt;
  readable = true;
  return character;
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
    bool readable = false;
    const auto character = ReadNeighbour(cookie, selection.Get(), before, readable);
    if (!readable) return {};
    if (character) (before ? hint.char_before : hint.char_after) = *character;
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

HRESULT SetCaret(ITfContext* context, TfEditCookie cookie, ITfRange* caret) {
  TF_SELECTION selection{};
  selection.range = caret;
  selection.style.ase = TF_AE_NONE;
  return context->SetSelection(cookie, 1, &selection);
}

// Collapses a clone of `written` to `offset` code units into the text that was
// just written there, using one of the three arrangements providers are known
// to leave that range in.
ComPtr<ITfRange> CaretCandidate(TfEditCookie cookie, ITfRange* written, int route, LONG forward,
                                LONG backward, HRESULT& error) {
  // A refused move reports E_FAIL, so the caller can still surface the more
  // specific failure of a provider that ran out of memory.
  error = E_FAIL;
  ComPtr<ITfRange> caret;
  const auto fail = [&](HRESULT hr) {
    error = FAILED(hr) ? hr : E_FAIL;
    return nullptr;
  };
  if (const HRESULT hr = written->Clone(&caret); FAILED(hr) || !caret) return fail(hr);
  LONG shifted = 0;
  switch (route) {
    case 0:  // The range covers the text, or stayed at its leading edge.
      if (const HRESULT hr = caret->Collapse(cookie, TF_ANCHOR_START); FAILED(hr)) return fail(hr);
      if (!forward) break;
      if (const HRESULT hr = caret->ShiftStart(cookie, forward, &shifted, nullptr);
          FAILED(hr) || shifted != forward)
        return fail(hr);
      break;
    case 1:  // The range stayed at the trailing edge of the written text.
      if (const HRESULT hr = caret->Collapse(cookie, TF_ANCHOR_END); FAILED(hr)) return fail(hr);
      if (!backward) break;
      if (const HRESULT hr = caret->ShiftStart(cookie, -backward, &shifted, nullptr);
          FAILED(hr) || shifted != -backward)
        return fail(hr);
      if (const HRESULT hr = caret->Collapse(cookie, TF_ANCHOR_START); FAILED(hr)) return fail(hr);
      break;
    default:  // As route 0, for a provider that only moves the end anchor.
      if (const HRESULT hr = caret->Collapse(cookie, TF_ANCHOR_START); FAILED(hr)) return fail(hr);
      if (!forward) break;
      if (const HRESULT hr = caret->ShiftEnd(cookie, forward, &shifted, nullptr);
          FAILED(hr) || shifted != forward)
        return fail(hr);
      if (const HRESULT hr = caret->Collapse(cookie, TF_ANCHOR_END); FAILED(hr)) return fail(hr);
      break;
  }
  error = S_OK;
  return caret;
}

// Confirms a candidate against the characters the write was supposed to leave
// around it. Unreadable neighbours stay kUnknown so a document that refuses
// GetText is not denied a caret it would otherwise have received.
enum class CaretCheck { kMatches, kUnknown, kMismatch };

CaretCheck CheckCaret(TfEditCookie cookie, ITfRange* caret, std::wstring_view written,
                      size_t offset) {
  auto verdict = CaretCheck::kMatches;
  for (const bool before : {true, false}) {
    if (before ? offset == 0 : offset >= written.size()) continue;
    const size_t index = before ? offset - 1 : offset;
    bool readable = false;
    const auto character = ReadNeighbour(cookie, caret, before, readable);
    if (!readable || !character) {
      verdict = CaretCheck::kUnknown;
      continue;
    }
    if (*character != written[index]) return CaretCheck::kMismatch;
  }
  return verdict;
}

// Places the caret `offset` code units into the text just written to `written`.
// Providers disagree on where SetText leaves that range - covering the text, or
// collapsed at either edge - and some refuse one shift direction outright, so
// every arrangement is tried and confirmed against the document instead of
// leaving the caret outside the pair that was inserted (DEV-1142).
HRESULT PlaceCaretAfterWrite(ITfContext* context, TfEditCookie cookie, ITfRange* written_range,
                             std::wstring_view written, size_t offset) {
  const LONG forward = static_cast<LONG>(offset);
  const LONG backward = static_cast<LONG>(written.size() - offset);
  constexpr int kTrailingEdgeRoute = 1;
  ComPtr<ITfRange> unconfirmed;
  ComPtr<ITfRange> unconfirmed_trailing;
  HRESULT failure = E_FAIL;
  for (int route = 0; route < 3; ++route) {
    HRESULT error = S_OK;
    auto caret = CaretCandidate(cookie, written_range, route, forward, backward, error);
    if (!caret) {
      if (failure == E_FAIL) failure = error;
      continue;
    }
    const auto check = CheckCaret(cookie, caret.Get(), written, offset);
    if (check == CaretCheck::kMatches) return SetCaret(context, cookie, caret.Get());
    if (check != CaretCheck::kUnknown) continue;
    if (route == kTrailingEdgeRoute)
      unconfirmed_trailing = std::move(caret);
    else if (!unconfirmed)
      unconfirmed = std::move(caret);
  }
  // No route matched the document. Move the caret only where nothing could be
  // read at all; guessing over a definite mismatch would select or split text.
  // A document that answers no read gets the trailing-edge route, which is
  // where the caret went before the other two existed: an unverifiable guess
  // must not land further from the pair than the previous behaviour did.
  if (unconfirmed_trailing) return SetCaret(context, cookie, unconfirmed_trailing.Get());
  if (unconfirmed) return SetCaret(context, cookie, unconfirmed.Get());
  return failure;
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
      action.type = ActionType::kInsertLiteral;
    ComPtr<ITfRange> range;
    HRESULT hr = Selection(context, cookie, range);
    if (FAILED(hr)) return hr;
    if (action.type == ActionType::kWrapSelection) {
      const auto literal = [&]() {
        const WCHAR character = static_cast<WCHAR>(action.open);
        const HRESULT write = range->SetText(cookie, 0, &character, 1);
        if (FAILED(write)) return write;
        applied = true;
        return PlaceCaretAfterWrite(context, cookie, range.Get(), {&character, 1}, 1);
      };
      ComPtr<ITfRange> reader;
      hr = range->Clone(&reader);
      if (FAILED(hr) || !reader) return literal();
      std::wstring text(1, static_cast<WCHAR>(action.open));
      std::array<WCHAR, 4096> buffer{};
      for (;;) {
        BOOL empty = FALSE;
        hr = reader->IsEmpty(cookie, &empty);
        if (FAILED(hr)) return literal();
        if (empty) break;
        ULONG count = 0;
        hr = reader->GetText(cookie, TF_TF_MOVESTART, buffer.data(),
                             static_cast<ULONG>(buffer.size()), &count);
        if (FAILED(hr)) return literal();
        if (!count || count > buffer.size() || text.size() + count > 65537) return literal();
        text.append(buffer.data(), count);
      }
      text.push_back(static_cast<WCHAR>(action.close));
      hr = range->SetText(cookie, 0, text.data(), static_cast<LONG>(text.size()));
      if (FAILED(hr)) return hr;
      applied = true;
      return PlaceCaretAfterWrite(context, cookie, range.Get(), text, text.size());
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
    const std::wstring_view written(text, pair ? 2 : 1);
    hr = range->SetText(cookie, 0, text, static_cast<LONG>(written.size()));
    if (FAILED(hr)) {
      if (composition) composition->EndComposition(cookie);
      return hr;
    }
    applied = true;  // A subsequent caret failure must not replay this insertion.
    if (composition) {
      service.composition_ = composition.Detach();
      service.bracket_composition_ = true;
    }
    // Between the pair, which for a single literal is past the one character.
    return PlaceCaretAfterWrite(context, cookie, range.Get(), written, 1);
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
