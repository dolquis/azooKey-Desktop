#include "azookey/tsf/ReconversionFunction.h"

#include <OleAuto.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <utility>

namespace azookey::tsf {
namespace {

template <typename Interface>
class RefCounted : public Interface {
 public:
  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
    if (!count) delete this;
    return count;
  }

 protected:
  virtual ~RefCounted() = default;

 private:
  LONG ref_count_{1};
};

class CandidateString final : public RefCounted<ITfCandidateString> {
 public:
  CandidateString(std::wstring value, ULONG index) : value_(std::move(value)), index_(index) {}

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfCandidateString) return E_NOINTERFACE;
    *out = static_cast<ITfCandidateString*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP GetString(BSTR* out) override {
    if (!out) return E_INVALIDARG;
    *out = SysAllocStringLen(value_.data(), static_cast<UINT>(value_.size()));
    return *out ? S_OK : E_OUTOFMEMORY;
  }
  STDMETHODIMP GetIndex(ULONG* out) override {
    if (!out) return E_INVALIDARG;
    *out = index_;
    return S_OK;
  }

 private:
  std::wstring value_;
  ULONG index_;
};

using CandidateValues = std::shared_ptr<const std::vector<std::wstring>>;

HRESULT ReplaceCandidate(ITfRange* range, TfClientId client_id, const std::wstring& original,
                         const std::wstring& replacement);

HRESULT MakeCandidate(const CandidateValues& values, ULONG index, ITfCandidateString** out) {
  if (!out) return E_INVALIDARG;
  *out = nullptr;
  if (index >= values->size()) return E_FAIL;
  auto* candidate = new (std::nothrow) CandidateString((*values)[index], index);
  if (!candidate) return E_OUTOFMEMORY;
  *out = candidate;
  return S_OK;
}

class CandidateEnum final : public RefCounted<IEnumTfCandidates> {
 public:
  CandidateEnum(CandidateValues values, ULONG index = 0)
      : values_(std::move(values)), index_(index) {}
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IEnumTfCandidates) return E_NOINTERFACE;
    *out = static_cast<IEnumTfCandidates*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP Clone(IEnumTfCandidates** out) override {
    if (!out) return E_INVALIDARG;
    *out = new (std::nothrow) CandidateEnum(values_, index_);
    return *out ? S_OK : E_OUTOFMEMORY;
  }
  STDMETHODIMP Next(ULONG count, ITfCandidateString** out, ULONG* fetched) override {
    if (!fetched || (count && !out)) return E_INVALIDARG;
    *fetched = 0;
    try {
      while (*fetched < count && index_ < values_->size()) {
        HRESULT hr = MakeCandidate(values_, index_, &out[*fetched]);
        if (FAILED(hr)) return hr;
        ++index_;
        ++*fetched;
      }
      return *fetched == count ? S_OK : S_FALSE;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }
  STDMETHODIMP Reset() override {
    index_ = 0;
    return S_OK;
  }
  STDMETHODIMP Skip(ULONG count) override {
    const size_t remaining = values_->size() - index_;
    const size_t skipped = std::min<size_t>(remaining, count);
    index_ += static_cast<ULONG>(skipped);
    return skipped == count ? S_OK : S_FALSE;
  }

 private:
  CandidateValues values_;
  ULONG index_;
};

class CandidateList final : public RefCounted<ITfCandidateList> {
 public:
  CandidateList(CandidateValues values, ITfRange* range, TfClientId client_id,
                std::wstring original)
      : values_(std::move(values)),
        range_(range),
        client_id_(client_id),
        original_(std::move(original)) {
    range_->AddRef();
  }
  ~CandidateList() override { range_->Release(); }
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfCandidateList) return E_NOINTERFACE;
    *out = static_cast<ITfCandidateList*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP EnumCandidates(IEnumTfCandidates** out) override {
    if (!out) return E_INVALIDARG;
    *out = new (std::nothrow) CandidateEnum(values_);
    return *out ? S_OK : E_OUTOFMEMORY;
  }
  STDMETHODIMP GetCandidate(ULONG index, ITfCandidateString** out) override {
    try {
      return MakeCandidate(values_, index, out);
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }
  STDMETHODIMP GetCandidateNum(ULONG* out) override {
    if (!out) return E_INVALIDARG;
    *out = static_cast<ULONG>(values_->size());
    return S_OK;
  }
  STDMETHODIMP SetResult(ULONG index, TfCandidateResult result) override {
    if (result == CAND_CANCELED) return S_OK;
    if (index >= values_->size()) return E_INVALIDARG;
    if (result == CAND_SELECTED) return S_OK;
    if (result != CAND_FINALIZED) return E_INVALIDARG;
    try {
      return ReplaceCandidate(range_, client_id_, original_, (*values_)[index]);
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

 private:
  CandidateValues values_;
  ITfRange* range_;
  TfClientId client_id_;
  std::wstring original_;
};

class FunctionEditSession final : public RefCounted<ITfEditSession> {
 public:
  explicit FunctionEditSession(std::function<HRESULT(TfEditCookie)> action)
      : action_(std::move(action)) {}
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfEditSession) return E_NOINTERFACE;
    *out = static_cast<ITfEditSession*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP DoEditSession(TfEditCookie cookie) override {
    try {
      return action_(cookie);
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

 private:
  std::function<HRESULT(TfEditCookie)> action_;
};

HRESULT WithEditSession(ITfRange* range, TfClientId client_id, DWORD flags,
                        const std::function<HRESULT(TfEditCookie)>& action) {
  ITfContext* context = nullptr;
  HRESULT hr = range->GetContext(&context);
  if (FAILED(hr)) return hr;
  if (!context) return E_FAIL;
  auto* edit = new (std::nothrow) FunctionEditSession(action);
  if (!edit) {
    context->Release();
    return E_OUTOFMEMORY;
  }
  HRESULT session_hr = E_FAIL;
  hr = context->RequestEditSession(client_id, edit, TF_ES_SYNC | flags, &session_hr);
  edit->Release();
  context->Release();
  if (FAILED(hr)) return hr;
  return session_hr == S_OK ? S_OK : (FAILED(session_hr) ? session_hr : E_FAIL);
}

HRESULT WithContextEditSession(ITfContext* context, TfClientId client_id, DWORD flags,
                               const std::function<HRESULT(TfEditCookie)>& action) {
  auto* edit = new (std::nothrow) FunctionEditSession(action);
  if (!edit) return E_OUTOFMEMORY;
  HRESULT session_hr = E_FAIL;
  const HRESULT hr = context->RequestEditSession(client_id, edit, TF_ES_SYNC | flags, &session_hr);
  edit->Release();
  if (FAILED(hr)) return hr;
  return session_hr == S_OK ? S_OK : (FAILED(session_hr) ? session_hr : E_FAIL);
}

constexpr ULONG kMaxSurfaceLength = 256;

HRESULT ReadSurface(ITfRange* range, TfEditCookie cookie, std::wstring& out) {
  out.clear();
  Microsoft::WRL::ComPtr<ITfRange> reader;
  HRESULT hr = range->Clone(&reader);
  if (FAILED(hr) || !reader) return FAILED(hr) ? hr : E_FAIL;
  while (true) {
    BOOL empty = FALSE;
    hr = reader->IsEmpty(cookie, &empty);
    if (FAILED(hr)) return hr;
    if (empty) return S_OK;
    if (out.size() >= kMaxSurfaceLength) return TF_E_NOCONVERSION;
    std::array<wchar_t, 64> buffer{};
    const ULONG capacity =
        static_cast<ULONG>((std::min)(buffer.size(), kMaxSurfaceLength - out.size()));
    ULONG length = 0;
    hr = reader->GetText(cookie, TF_TF_MOVESTART, buffer.data(), capacity, &length);
    if (FAILED(hr)) return hr;
    if (!length || length > capacity) return E_FAIL;
    out.append(buffer.data(), length);
  }
}

bool IsJapanese(wchar_t ch) {
  return (ch >= 0x3040 && ch <= 0x30ff) || (ch >= 0x3400 && ch <= 0x9fff) ||
         (ch >= 0xff66 && ch <= 0xff9f);
}

HRESULT ReadRangeSurface(ITfRange* range, TfClientId client_id, std::wstring& surface) {
  return WithEditSession(range, client_id, TF_ES_READ,
                         [&](TfEditCookie cookie) { return ReadSurface(range, cookie, surface); });
}

HRESULT ReplaceCandidate(ITfRange* range, TfClientId client_id, const std::wstring& original,
                         const std::wstring& replacement) {
  return WithEditSession(range, client_id, TF_ES_READWRITE, [&](TfEditCookie cookie) {
    std::wstring current;
    HRESULT hr = ReadSurface(range, cookie, current);
    if (FAILED(hr)) return hr;
    if (current != original) return TF_E_NOCONVERSION;
    return range->SetText(cookie, 0, replacement.data(), static_cast<LONG>(replacement.size()));
  });
}

}  // namespace

ReconversionFunction::ReconversionFunction(TfClientId client_id,
                                           ReconversionCandidateProvider provider)
    : client_id_(client_id), provider_(std::move(provider)) {}

STDMETHODIMP ReconversionFunction::QueryInterface(REFIID iid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (iid == IID_IUnknown || iid == IID_ITfFunction || iid == IID_ITfFnReconversion)
    *out = static_cast<ITfFnReconversion*>(this);
  else
    return E_NOINTERFACE;
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) ReconversionFunction::AddRef() {
  return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
}

STDMETHODIMP_(ULONG) ReconversionFunction::Release() {
  const ULONG count = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  if (!count) delete this;
  return count;
}

STDMETHODIMP ReconversionFunction::GetDisplayName(BSTR* name) {
  if (!name) return E_INVALIDARG;
  *name = SysAllocString(L"azooKey Reconversion");
  return *name ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP ReconversionFunction::QueryRange(ITfRange* range, ITfRange** new_range,
                                              BOOL* convertible) {
  if (!convertible || !range) return E_INVALIDARG;
  *convertible = FALSE;
  if (new_range) *new_range = nullptr;
  ITfRange* result = nullptr;
  try {
    const HRESULT hr = WithEditSession(range, client_id_, TF_ES_READ, [&](TfEditCookie cookie) {
      HRESULT clone_hr = range->Clone(&result);
      if (FAILED(clone_hr)) return clone_hr;
      if (!result) return E_FAIL;
      BOOL empty = FALSE;
      clone_hr = result->IsEmpty(cookie, &empty);
      if (FAILED(clone_hr)) return clone_hr;
      if (empty) {
        LONG left = 0, right = 0;
        clone_hr = result->ShiftStart(cookie, -8, &left, nullptr);
        if (FAILED(clone_hr)) return clone_hr;
        clone_hr = result->ShiftEnd(cookie, 8, &right, nullptr);
        if (FAILED(clone_hr)) return clone_hr;
        std::wstring text;
        clone_hr = ReadSurface(result, cookie, text);
        if (FAILED(clone_hr)) return clone_hr;
        const size_t caret = static_cast<size_t>(-left);
        if (caret > text.size()) return E_FAIL;
        size_t begin = caret;
        size_t end = caret;
        while (begin && IsJapanese(text[begin - 1])) --begin;
        while (end < text.size() && IsJapanese(text[end])) ++end;
        if (begin == end) return S_OK;
        LONG moved = 0;
        clone_hr = result->ShiftStart(cookie, static_cast<LONG>(begin), &moved, nullptr);
        if (FAILED(clone_hr)) return clone_hr;
        clone_hr = result->ShiftEnd(cookie, -static_cast<LONG>(text.size() - end), &moved, nullptr);
        if (FAILED(clone_hr)) return clone_hr;
      } else {
        std::wstring text;
        clone_hr = ReadSurface(result, cookie, text);
        if (FAILED(clone_hr)) return clone_hr;
        if (std::none_of(text.begin(), text.end(), IsJapanese)) return S_OK;
      }
      *convertible = TRUE;
      return S_OK;
    });
    if (FAILED(hr) || !*convertible) {
      if (result) result->Release();
      return hr;
    }
    if (new_range)
      *new_range = result;
    else
      result->Release();
    return S_OK;
  } catch (const std::bad_alloc&) {
    if (result) result->Release();
    return E_OUTOFMEMORY;
  } catch (...) {
    if (result) result->Release();
    return E_FAIL;
  }
}

STDMETHODIMP ReconversionFunction::GetReconversion(ITfRange* range, ITfCandidateList** candidates) {
  if (!range || !candidates) return E_INVALIDARG;
  *candidates = nullptr;
  try {
    std::wstring surface;
    HRESULT hr = ReadRangeSurface(range, client_id_, surface);
    if (FAILED(hr)) return hr;
    if (surface.empty() || !provider_) return TF_E_NOCONVERSION;
    std::vector<std::wstring> values;
    hr = provider_(range, surface, values);
    if (FAILED(hr)) return hr;
    if (values.empty()) return TF_E_NOCONVERSION;
    auto snapshot = std::make_shared<const std::vector<std::wstring>>(std::move(values));
    *candidates = new (std::nothrow)
        CandidateList(std::move(snapshot), range, client_id_, std::move(surface));
    return *candidates ? S_OK : E_OUTOFMEMORY;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

STDMETHODIMP ReconversionFunction::Reconvert(ITfRange* range) {
  if (!range) return E_INVALIDARG;
  try {
    std::wstring surface;
    HRESULT hr = ReadRangeSurface(range, client_id_, surface);
    if (FAILED(hr)) return hr;
    if (surface.empty() || !provider_) return TF_E_NOCONVERSION;
    std::vector<std::wstring> candidates;
    hr = provider_(range, surface, candidates);
    if (FAILED(hr)) return hr;
    if (candidates.empty()) return TF_E_NOCONVERSION;
    return ReplaceCandidate(range, client_id_, surface, candidates[0]);
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

HRESULT ReconversionFunction::ReconvertSelection(ITfContext* context) {
  ITfRange* target = nullptr;
  std::wstring surface;
  const HRESULT hr = CaptureSelection(context, &target, surface);
  if (FAILED(hr)) return hr;
  const HRESULT reconvert_hr = Reconvert(target);
  target->Release();
  return reconvert_hr;
}

HRESULT ReconversionFunction::CaptureSelection(ITfContext* context, ITfRange** range,
                                               std::wstring& surface) {
  if (!context) return E_INVALIDARG;
  if (!range) return E_INVALIDARG;
  *range = nullptr;
  surface.clear();
  ITfRange* selection = nullptr;
  try {
    const HRESULT hr =
        WithContextEditSession(context, client_id_, TF_ES_READ, [&](TfEditCookie cookie) {
          TF_SELECTION selected{};
          ULONG fetched = 0;
          const HRESULT selection_hr =
              context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selected, &fetched);
          if (FAILED(selection_hr)) return selection_hr;
          if (fetched != 1 || !selected.range) return TF_E_NOCONVERSION;
          selection = selected.range;
          return S_OK;
        });
    if (FAILED(hr)) return hr;
    BOOL convertible = FALSE;
    const HRESULT query_hr = QueryRange(selection, range, &convertible);
    selection->Release();
    selection = nullptr;
    if (FAILED(query_hr)) return query_hr;
    if (!convertible || !*range) return TF_E_NOCONVERSION;
    const HRESULT read_hr = ReadRangeSurface(*range, client_id_, surface);
    if (FAILED(read_hr) || surface.empty()) {
      (*range)->Release();
      *range = nullptr;
      return FAILED(read_hr) ? read_hr : TF_E_NOCONVERSION;
    }
    return S_OK;
  } catch (const std::bad_alloc&) {
    if (selection) selection->Release();
    if (*range) {
      (*range)->Release();
      *range = nullptr;
    }
    return E_OUTOFMEMORY;
  } catch (...) {
    if (selection) selection->Release();
    if (*range) {
      (*range)->Release();
      *range = nullptr;
    }
    return E_FAIL;
  }
}

}  // namespace azookey::tsf
