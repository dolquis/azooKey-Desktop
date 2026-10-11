#include "azookey/tsf/ProofreadDocumentReader.h"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <new>
#include <utility>

#include "azookey/tsf/TipRuntimeLog.h"

namespace azookey::tsf {
namespace {

constexpr size_t kReadChunkCodeUnits = 4096;

bool IsHighSurrogate(wchar_t value) { return value >= 0xd800 && value <= 0xdbff; }
bool IsLowSurrogate(wchar_t value) { return value >= 0xdc00 && value <= 0xdfff; }

size_t Utf8Bytes(wchar_t value) {
  if (value < 0x80) return 1;
  if (value < 0x800) return 2;
  return 3;  // BMP, or an unpaired surrogate written as U+FFFD.
}

// Reads `range` to its end with TF_TF_MOVESTART, at most `limit` code units.
HRESULT ReadToEnd(ITfRange* range, TfEditCookie cookie, size_t limit, std::wstring& out) {
  std::array<wchar_t, kReadChunkCodeUnits> buffer{};
  while (out.size() < limit) {
    BOOL empty = FALSE;
    HRESULT hr = range->IsEmpty(cookie, &empty);
    if (FAILED(hr)) return hr;
    if (empty) return S_OK;
    const ULONG capacity = static_cast<ULONG>((std::min)(buffer.size(), limit - out.size()));
    ULONG read = 0;
    hr = range->GetText(cookie, TF_TF_MOVESTART, buffer.data(), capacity, &read);
    if (FAILED(hr) || !read || read > capacity) return E_FAIL;
    out.append(buffer.data(), read);
  }
  return S_OK;
}

class ReadSession final : public ITfEditSession {
 public:
  explicit ReadSession(ITfContext* context) : context_(context) {}

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfEditSession) return E_NOINTERFACE;
    *out = static_cast<ITfEditSession*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refs_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG refs = static_cast<ULONG>(InterlockedDecrement(&refs_));
    if (!refs) delete this;
    return refs;
  }
  STDMETHODIMP DoEditSession(TfEditCookie cookie) override {
    try {
      Microsoft::WRL::ComPtr<ITfRange> anchor;
      TF_SELECTION selection{};
      ULONG fetched = 0;
      const HRESULT selection_hr =
          context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
      if (SUCCEEDED(selection_hr) && fetched == 1 && selection.range) {
        anchor.Attach(selection.range);
      } else {
        if (selection.range) selection.range->Release();
        // No selection: check the document from its start.
        if (FAILED(context_->GetStart(cookie, &anchor)) || !anchor) return E_FAIL;
      }
      HRESULT hr = anchor->Collapse(cookie, TF_ANCHOR_START);
      if (FAILED(hr)) return hr;

      Microsoft::WRL::ComPtr<ITfRange> before;
      Microsoft::WRL::ComPtr<ITfRange> after;
      if (FAILED(anchor->Clone(&before)) || !before || FAILED(anchor->Clone(&after)) || !after)
        return E_FAIL;
      constexpr LONG kReach = static_cast<LONG>(kProofreadReadCodeUnits);
      LONG shifted = 0;
      hr = before->ShiftStart(cookie, -kReach, &shifted, nullptr);
      if (FAILED(hr) || shifted > 0 || shifted < -kReach) return E_FAIL;
      hr = after->ShiftEnd(cookie, kReach, &shifted, nullptr);
      if (FAILED(hr) || shifted < 0 || shifted > kReach) return E_FAIL;

      std::wstring text;
      hr = ReadToEnd(before.Get(), cookie, kProofreadReadCodeUnits, text);
      if (FAILED(hr)) return hr;
      const size_t anchor_offset = text.size();
      std::wstring tail;
      hr = ReadToEnd(after.Get(), cookie, kProofreadReadCodeUnits, tail);
      if (FAILED(hr)) return hr;
      text += tail;

      const auto window = SelectProofreadWindow(text, anchor_offset, ipc::kMaxAnomalyTextBytes);
      ProofreadDocument document;
      document.truncated = window.length < text.size();
      document.text = text.substr(window.start, window.length);
      document.anchor =
          (std::min)(anchor_offset - (std::min)(anchor_offset, window.start), document.text.size());
      if (!document.text.empty()) {
        hr = anchor->Clone(&document.range);
        if (FAILED(hr) || !document.range) return E_FAIL;
        const LONG back = static_cast<LONG>(document.anchor);
        const LONG ahead = static_cast<LONG>(document.text.size() - document.anchor);
        if (FAILED(document.range->ShiftStart(cookie, -back, &shifted, nullptr)) ||
            shifted != -back ||
            FAILED(document.range->ShiftEnd(cookie, ahead, &shifted, nullptr)) || shifted != ahead)
          return E_FAIL;
      }
      result_ = std::move(document);
      return S_OK;
    } catch (...) {
      LogComBoundaryException("ProofreadDocumentReader::DoEditSession", E_FAIL);
      result_.reset();
      return E_FAIL;
    }
  }

  std::optional<ProofreadDocument>& result() { return result_; }

 private:
  ~ReadSession() = default;

  LONG refs_{1};
  Microsoft::WRL::ComPtr<ITfContext> context_;
  std::optional<ProofreadDocument> result_;
};

}  // namespace

ProofreadWindow SelectProofreadWindow(std::wstring_view text, size_t anchor,
                                      size_t max_utf8_bytes) noexcept {
  anchor = (std::min)(anchor, text.size());
  // Never start inside a surrogate pair.
  if (anchor > 0 && anchor < text.size() && IsLowSurrogate(text[anchor]) &&
      IsHighSurrogate(text[anchor - 1]))
    --anchor;
  size_t left = anchor;
  size_t right = anchor;
  size_t bytes = 0;
  bool grow_right = true;
  bool grow_left = true;
  while (grow_right || grow_left) {
    if (grow_right) {
      if (right >= text.size()) {
        grow_right = false;
      } else {
        const bool pair = IsHighSurrogate(text[right]) && right + 1 < text.size() &&
                          IsLowSurrogate(text[right + 1]);
        const size_t cost = pair ? 4 : Utf8Bytes(text[right]);
        if (bytes + cost > max_utf8_bytes) {
          grow_right = false;
        } else {
          bytes += cost;
          right += pair ? 2 : 1;
        }
      }
    }
    if (grow_left) {
      if (left == 0) {
        grow_left = false;
      } else {
        const bool pair =
            left >= 2 && IsLowSurrogate(text[left - 1]) && IsHighSurrogate(text[left - 2]);
        const size_t cost = pair ? 4 : Utf8Bytes(text[left - 1]);
        if (bytes + cost > max_utf8_bytes) {
          grow_left = false;
        } else {
          bytes += cost;
          left -= pair ? 2 : 1;
        }
      }
    }
  }
  return {left, right - left};
}

std::optional<ProofreadDocument> ReadProofreadDocument(ITfContext* context,
                                                       TfClientId client_id) noexcept {
  if (!context) return std::nullopt;
  try {
    auto* session = new (std::nothrow) ReadSession(context);
    if (!session) return std::nullopt;
    Microsoft::WRL::ComPtr<ITfEditSession> session_owner;
    session_owner.Attach(session);
    HRESULT edit_result = E_FAIL;
    const HRESULT hr =
        context->RequestEditSession(client_id, session, TF_ES_SYNC | TF_ES_READ, &edit_result);
    if (FAILED(hr) || edit_result != S_OK) return std::nullopt;
    auto& result = session->result();
    if (!result || result->text.empty()) return std::nullopt;
    return std::move(result);
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace azookey::tsf
