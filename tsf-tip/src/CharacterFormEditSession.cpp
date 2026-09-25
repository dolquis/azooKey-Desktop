#include "azookey/tsf/CharacterFormEditSession.h"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace azookey::tsf {
namespace {

using Microsoft::WRL::ComPtr;
constexpr size_t kMaxSelectedCodeUnits = 4096;

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
  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&references_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&references_));
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
  session.Attach(new (std::nothrow) SynchronousSession(std::move(body)));
  if (!session) return E_OUTOFMEMORY;
  HRESULT session_result = E_FAIL;
  const HRESULT request =
      context->RequestEditSession(client_id, session.Get(), TF_ES_SYNC | access, &session_result);
  // A provider retaining a synchronous session cannot later use stack state.
  session->Disarm();
  if (FAILED(request)) return request;
  return session->executed ? session_result : E_FAIL;
}

// GetText may return only part of a range. Advance a clone and require its end
// to be reached before any write; partial reads must never delete the rest.
HRESULT ReadSelectedText(ITfRange* range, TfEditCookie cookie, std::wstring& text) {
  ComPtr<ITfRange> reader;
  HRESULT hr = range->Clone(&reader);
  if (FAILED(hr) || !reader) return FAILED(hr) ? hr : E_FAIL;
  while (text.size() <= kMaxSelectedCodeUnits) {
    BOOL empty = FALSE;
    hr = reader->IsEmpty(cookie, &empty);
    if (FAILED(hr)) return hr;
    if (empty) return text.empty() ? S_FALSE : S_OK;
    if (text.size() == kMaxSelectedCodeUnits) return S_FALSE;
    std::array<WCHAR, 256> buffer{};
    ULONG count = 0;
    const ULONG capacity =
        static_cast<ULONG>((std::min)(buffer.size(), kMaxSelectedCodeUnits - text.size()));
    hr = reader->GetText(cookie, TF_TF_MOVESTART, buffer.data(), capacity, &count);
    if (FAILED(hr)) return hr;
    if (count == 0 || count > capacity) return E_FAIL;
    text.append(buffer.data(), count);
  }
  return S_FALSE;
}

HRESULT RangeBeforeCaret(ITfRange* caret, TfEditCookie cookie, size_t length,
                         ComPtr<ITfRange>& range) {
  if (length == 0 || length > kMaxSelectedCodeUnits) return S_FALSE;
  HRESULT hr = caret->Clone(&range);
  if (FAILED(hr) || !range) return FAILED(hr) ? hr : E_FAIL;
  LONG shifted = 0;
  hr = range->ShiftStart(cookie, -static_cast<LONG>(length), &shifted, nullptr);
  if (FAILED(hr)) return hr;
  return shifted == -static_cast<LONG>(length) ? S_OK : S_FALSE;
}

bool SameComIdentity(ITfContext* left, ITfContext* right) {
  if (!left || !right) return false;
  ComPtr<IUnknown> left_identity;
  ComPtr<IUnknown> right_identity;
  return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&left_identity))) &&
         SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&right_identity))) &&
         left_identity.Get() == right_identity.Get();
}

std::optional<std::string> ToUtf8(const std::wstring& wide) {
  if (wide.empty() || wide.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
    return std::nullopt;
  const int length =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
                          nullptr, 0, nullptr, nullptr);
  if (length <= 0) return std::nullopt;
  std::string result(static_cast<size_t>(length), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
                          result.data(), length, nullptr, nullptr) != length)
    return std::nullopt;
  return result;
}

std::optional<std::wstring> ToWide(const std::string& utf8) {
  if (utf8.empty() || utf8.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
    return std::nullopt;
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  if (length <= 0) return std::nullopt;
  std::wstring result(static_cast<size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                          result.data(), length) != length)
    return std::nullopt;
  return result;
}

HRESULT CycleAtCookie(ITfContext* context, TfEditCookie cookie,
                      const std::optional<CharacterFormCycleState>& previous,
                      CharacterFormCycleResult& result) {
  TF_SELECTION selection{};
  ULONG fetched = 0;
  HRESULT hr = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
  ComPtr<ITfRange> range;
  range.Attach(selection.range);
  if (FAILED(hr)) return hr;
  if (fetched != 1 || !range) return S_FALSE;
  BOOL empty = FALSE;
  hr = range->IsEmpty(cookie, &empty);
  if (FAILED(hr)) return hr;
  if (empty) return S_FALSE;

  std::wstring selected;
  hr = ReadSelectedText(range.Get(), cookie, selected);
  if (hr != S_OK) return hr;
  const auto utf8 = ToUtf8(selected);
  if (!utf8) return S_FALSE;

  CharacterFormCycleState next;
  if (previous && previous->index < previous->cycle.forms.size() &&
      previous->cycle.forms[previous->index] == *utf8) {
    next = *previous;
    next.index = (next.index + 1) % next.cycle.forms.size();
  } else {
    const auto cycle = core::BuildCharacterFormCycle(*utf8);
    if (!cycle) return S_FALSE;
    next.cycle = *cycle;
    next.index = core::CharacterFormCycle::kKatakana;
  }

  const auto replacement = ToWide(next.cycle.forms[next.index]);
  if (!replacement || replacement->size() > static_cast<size_t>((std::numeric_limits<LONG>::max)()))
    return E_FAIL;

  // Query before writing. InsertTextAtSelection replaces the current selection
  // and returns the inserted range, independent of how SetText moves its input.
  ComPtr<ITfInsertAtSelection> insert;
  hr = context->QueryInterface(IID_PPV_ARGS(&insert));
  if (FAILED(hr) || !insert) return FAILED(hr) ? hr : E_NOINTERFACE;
  ComPtr<ITfRange> inserted;
  hr = insert->InsertTextAtSelection(cookie, 0, replacement->data(),
                                     static_cast<LONG>(replacement->size()), &inserted);
  if (FAILED(hr)) return hr;
  result.applied = true;
  if (!inserted) return E_FAIL;

  BOOL inserted_empty = TRUE;
  hr = inserted->IsEmpty(cookie, &inserted_empty);
  if (FAILED(hr)) return hr;
  if (inserted_empty) return E_FAIL;
  TF_SELECTION restored{};
  restored.range = inserted.Get();
  restored.style = selection.style;
  restored.style.fInterimChar = FALSE;
  hr = context->SetSelection(cookie, 1, &restored);
  if (FAILED(hr)) return hr;
  result.state = std::move(next);
  return S_OK;
}

}  // namespace

bool CharacterFormEditSession::CanCycleSelection(
    ITfContext* context, TfClientId client_id,
    const std::optional<CharacterFormCycleState>& previous) {
  if (!context) return false;
  try {
    bool supported = false;
    const HRESULT hr = RunSync(context, client_id, TF_ES_READ, [&](TfEditCookie cookie) {
      TF_SELECTION selection{};
      ULONG fetched = 0;
      HRESULT read_hr =
          context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
      ComPtr<ITfRange> range;
      range.Attach(selection.range);
      if (FAILED(read_hr) || fetched != 1 || !range) return S_FALSE;
      BOOL empty = TRUE;
      read_hr = range->IsEmpty(cookie, &empty);
      if (FAILED(read_hr) || empty) return S_FALSE;
      std::wstring selected;
      read_hr = ReadSelectedText(range.Get(), cookie, selected);
      if (read_hr != S_OK) return S_FALSE;
      const auto utf8 = ToUtf8(selected);
      if (!utf8) return S_FALSE;
      supported = (previous && previous->index < previous->cycle.forms.size() &&
                   previous->cycle.forms[previous->index] == *utf8) ||
                  core::BuildCharacterFormCycle(*utf8).has_value();
      return supported ? S_OK : S_FALSE;
    });
    return hr == S_OK && supported;
  } catch (...) {
    return false;
  }
}

CharacterFormCycleResult CharacterFormEditSession::CycleSelection(
    ITfContext* context, TfClientId client_id,
    const std::optional<CharacterFormCycleState>& previous) {
  CharacterFormCycleResult result;
  if (!context) {
    result.status = E_INVALIDARG;
    return result;
  }
  try {
    result.status = RunSync(context, client_id, TF_ES_READWRITE, [&](TfEditCookie cookie) {
      return CycleAtCookie(context, cookie, previous, result);
    });
  } catch (const std::bad_alloc&) {
    result.status = E_OUTOFMEMORY;
  } catch (...) {
    result.status = E_FAIL;
  }
  return result;
}

std::optional<CharacterFormRecentCommit> CharacterFormEditSession::CaptureRecentCommit(
    ITfContext* context, TfClientId client_id, std::string_view reading,
    std::string_view committed_surface) {
  if (!context || committed_surface.empty()) return std::nullopt;
  try {
    const auto cycle = core::BuildCharacterFormCycle(reading);
    if (!cycle) return std::nullopt;
    const auto surface = ToWide(std::string(committed_surface));
    if (!surface || surface->empty() || surface->size() > kMaxSelectedCodeUnits)
      return std::nullopt;
    std::optional<CharacterFormRecentCommit> captured;
    const HRESULT hr = RunSync(context, client_id, TF_ES_READ, [&](TfEditCookie cookie) {
      TF_SELECTION selection{};
      ULONG fetched = 0;
      HRESULT read_hr =
          context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
      ComPtr<ITfRange> caret;
      caret.Attach(selection.range);
      if (FAILED(read_hr)) return read_hr;
      if (fetched != 1 || !caret) return S_FALSE;
      BOOL empty = FALSE;
      read_hr = caret->IsEmpty(cookie, &empty);
      if (FAILED(read_hr)) return read_hr;
      if (!empty) return S_FALSE;
      ComPtr<ITfRange> committed;
      read_hr = RangeBeforeCaret(caret.Get(), cookie, surface->size(), committed);
      if (read_hr != S_OK) return read_hr;
      std::wstring actual;
      read_hr = ReadSelectedText(committed.Get(), cookie, actual);
      if (read_hr != S_OK) return read_hr;
      if (actual != *surface) return S_FALSE;
      captured.emplace();
      captured->context = context;
      captured->caret = caret;
      captured->cycle = *cycle;
      captured->surface = *surface;
      captured->next_index =
          committed_surface == std::string_view(cycle->forms[core::CharacterFormCycle::kHiragana])
              ? core::CharacterFormCycle::kKatakana
              : core::CharacterFormCycle::kHiragana;
      return S_OK;
    });
    return hr == S_OK ? captured : std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

CharacterFormRecentCommitResult CharacterFormEditSession::CycleRecentCommit(
    ITfContext* context, TfClientId client_id, const CharacterFormRecentCommit& previous) {
  CharacterFormRecentCommitResult result;
  if (!context) {
    result.status = E_INVALIDARG;
    return result;
  }
  if (!previous.caret || !SameComIdentity(context, previous.context.Get()) ||
      previous.next_index >= previous.cycle.forms.size())
    return result;
  try {
    const auto replacement = ToWide(previous.cycle.forms[previous.next_index]);
    if (!replacement || replacement->empty() ||
        replacement->size() > static_cast<size_t>((std::numeric_limits<LONG>::max)())) {
      result.status = E_FAIL;
      return result;
    }
    result.status = RunSync(context, client_id, TF_ES_READWRITE, [&](TfEditCookie cookie) {
      TF_SELECTION original{};
      ULONG fetched = 0;
      HRESULT hr = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &original, &fetched);
      ComPtr<ITfRange> caret;
      caret.Attach(original.range);
      if (FAILED(hr)) return hr;
      if (fetched != 1 || !caret) return S_FALSE;
      BOOL empty = FALSE;
      hr = caret->IsEmpty(cookie, &empty);
      if (FAILED(hr)) return hr;
      if (!empty) return S_FALSE;
      BOOL at_commit = FALSE;
      hr = caret->IsEqualStart(cookie, previous.caret.Get(), TF_ANCHOR_START, &at_commit);
      if (FAILED(hr)) return hr;
      if (!at_commit) return S_FALSE;

      ComPtr<ITfRange> committed;
      hr = RangeBeforeCaret(caret.Get(), cookie, previous.surface.size(), committed);
      if (hr != S_OK) return hr;
      std::wstring actual;
      hr = ReadSelectedText(committed.Get(), cookie, actual);
      if (hr != S_OK) return hr;
      if (actual != previous.surface) return S_FALSE;

      ComPtr<ITfInsertAtSelection> insert;
      hr = context->QueryInterface(IID_PPV_ARGS(&insert));
      if (FAILED(hr) || !insert) return FAILED(hr) ? hr : E_NOINTERFACE;
      TF_SELECTION target{};
      target.range = committed.Get();
      target.style = original.style;
      target.style.fInterimChar = FALSE;
      hr = context->SetSelection(cookie, 1, &target);
      if (hr != S_OK) {
        if (context->SetSelection(cookie, 1, &original) != S_OK) result.consume_key = true;
        return FAILED(hr) ? hr : E_FAIL;
      }
      result.consume_key = true;

      ComPtr<ITfRange> inserted;
      hr = insert->InsertTextAtSelection(cookie, 0, replacement->data(),
                                         static_cast<LONG>(replacement->size()), &inserted);
      if (hr != S_OK) {
        if (context->SetSelection(cookie, 1, &original) == S_OK) result.consume_key = false;
        return FAILED(hr) ? hr : E_FAIL;
      }
      result.replaced = true;
      if (!inserted) return E_FAIL;
      BOOL inserted_empty = TRUE;
      hr = inserted->IsEmpty(cookie, &inserted_empty);
      if (FAILED(hr)) return hr;
      if (inserted_empty) return E_FAIL;
      ComPtr<ITfRange> next_caret;
      hr = inserted->Clone(&next_caret);
      if (FAILED(hr) || !next_caret) return FAILED(hr) ? hr : E_FAIL;
      hr = next_caret->Collapse(cookie, TF_ANCHOR_END);
      if (FAILED(hr)) return hr;
      TF_SELECTION finished{};
      finished.range = next_caret.Get();
      finished.style.ase = TF_AE_NONE;
      hr = context->SetSelection(cookie, 1, &finished);
      if (hr != S_OK) return FAILED(hr) ? hr : E_FAIL;

      result.state = previous;
      result.state->caret = next_caret;
      result.state->surface = *replacement;
      result.state->next_index = (previous.next_index + 1) % previous.cycle.forms.size();
      return S_OK;
    });
  } catch (const std::bad_alloc&) {
    result.status = E_OUTOFMEMORY;
  } catch (...) {
    result.status = E_FAIL;
  }
  return result;
}

}  // namespace azookey::tsf
