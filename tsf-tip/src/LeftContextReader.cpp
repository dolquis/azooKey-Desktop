#include "azookey/tsf/LeftContextReader.h"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <new>
#include <string>

#include "azookey/tsf/TipRuntimeLog.h"

namespace azookey::tsf {
namespace {

constexpr size_t kMaxContextCodepoints = 30;
constexpr size_t kReadCodeUnits = kMaxContextCodepoints * 2 + 4;

bool IsHighSurrogate(wchar_t value) { return value >= 0xd800 && value <= 0xdbff; }
bool IsLowSurrogate(wchar_t value) { return value >= 0xdc00 && value <= 0xdfff; }

bool HasSameContext(ITfContext* context, ITfRange* range) {
  Microsoft::WRL::ComPtr<ITfContext> range_context;
  if (FAILED(range->GetContext(&range_context)) || !range_context) return false;
  Microsoft::WRL::ComPtr<IUnknown> expected;
  Microsoft::WRL::ComPtr<IUnknown> actual;
  return SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&expected))) && expected &&
         SUCCEEDED(range_context->QueryInterface(IID_PPV_ARGS(&actual))) && actual &&
         expected.Get() == actual.Get();
}

class ReadSession final : public ITfEditSession {
 public:
  ReadSession(ITfContext* context, ITfRange* composition_range, size_t max_codepoints)
      : context_(context), composition_range_(composition_range), max_codepoints_(max_codepoints) {}

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
      Microsoft::WRL::ComPtr<ITfRange> range;
      HRESULT hr = S_OK;
      if (composition_range_) {
        hr = composition_range_->Clone(&range);
      } else {
        TF_SELECTION selection{};
        ULONG fetched = 0;
        hr = context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
        if (SUCCEEDED(hr) && fetched == 1)
          range.Attach(selection.range);
        else if (selection.range)
          selection.range->Release();
      }
      if (FAILED(hr) || !range) return E_FAIL;
      return ReadRange(range.Get(), cookie);
    } catch (...) {
      LogComBoundaryException("LeftContextReader::DoEditSession", E_FAIL);
      result_.clear();
      return E_FAIL;
    }
  }

  const std::string& result() const { return result_; }

 private:
  ~ReadSession() = default;

  HRESULT ReadRange(ITfRange* range, TfEditCookie cookie) {
    HRESULT hr = range->Collapse(cookie, TF_ANCHOR_START);
    if (FAILED(hr)) return hr;
    LONG shifted = 0;
    hr = range->ShiftStart(cookie, -static_cast<LONG>(kReadCodeUnits), &shifted, nullptr);
    if (FAILED(hr) || shifted > 0 || shifted < -static_cast<LONG>(kReadCodeUnits)) return E_FAIL;

    std::wstring wide;
    while (true) {
      BOOL empty = FALSE;
      hr = range->IsEmpty(cookie, &empty);
      if (FAILED(hr)) return hr;
      if (empty) break;
      std::array<wchar_t, kReadCodeUnits> buffer{};
      ULONG read = 0;
      const ULONG capacity = static_cast<ULONG>(buffer.size() - wide.size());
      if (!capacity) return E_FAIL;
      hr = range->GetText(cookie, TF_TF_MOVESTART, buffer.data(), capacity, &read);
      if (FAILED(hr) || !read || read > capacity) return E_FAIL;
      wide.append(buffer.data(), read);
    }
    const auto line_start = wide.find_last_of(L"\r\n");
    if (line_start != std::wstring::npos) wide.erase(0, line_start + 1);
    size_t start = wide.size();
    for (size_t count = 0; start && count < max_codepoints_; ++count) {
      --start;
      if (IsLowSurrogate(wide[start])) {
        if (!start || !IsHighSurrogate(wide[start - 1])) return E_FAIL;
        --start;
      } else if (IsHighSurrogate(wide[start])) {
        return E_FAIL;
      }
    }
    wide.erase(0, start);
    if (wide.empty()) return S_OK;
    const int length =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return E_FAIL;
    result_.resize(static_cast<size_t>(length));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), result_.data(), length, nullptr,
                            nullptr) != length) {
      result_.clear();
      return E_FAIL;
    }
    return S_OK;
  }

  LONG refs_{1};
  Microsoft::WRL::ComPtr<ITfContext> context_;
  Microsoft::WRL::ComPtr<ITfRange> composition_range_;
  size_t max_codepoints_;
  std::string result_;
};

}  // namespace

std::string ReadLeftContext(ITfContext* context, ITfRange* composition_range, TfClientId client_id,
                            size_t max_codepoints) noexcept {
  if (!context || !max_codepoints) return {};
  try {
    if (composition_range && !HasSameContext(context, composition_range)) return {};
    auto* session = new (std::nothrow)
        ReadSession(context, composition_range, (std::min)(max_codepoints, kMaxContextCodepoints));
    if (!session) return {};
    Microsoft::WRL::ComPtr<ITfEditSession> session_owner;
    session_owner.Attach(session);
    HRESULT edit_result = E_FAIL;
    const HRESULT hr =
        context->RequestEditSession(client_id, session, TF_ES_SYNC | TF_ES_READ, &edit_result);
    std::string result;
    if (SUCCEEDED(hr) && edit_result == S_OK) result = session->result();
    return result;
  } catch (...) {
    return {};
  }
}

}  // namespace azookey::tsf
