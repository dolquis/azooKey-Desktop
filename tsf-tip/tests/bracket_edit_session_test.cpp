#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <gtest/gtest.h>
#include <msctf.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "azookey/core/BracketPairing.h"
#include "azookey/core/BracketSettings.h"
#include "azookey/tsf/BracketEditSession.h"
#include "azookey/tsf/TextService.h"

namespace {

using azookey::core::BracketPairingAction;
using azookey::core::BracketPairingActionType;
using azookey::core::BracketPairingTrigger;
using azookey::core::BracketSettings;
using azookey::tsf::BracketEditSession;

// Injected failures live on the document so that a fault survives the Clone()
// that ReadAtCookie, PlaceCaret and the wrap reader each perform.
struct RangeFaults {
  HRESULT clone{S_OK};
  HRESULT collapse{S_OK};
  HRESULT shift_start{S_OK};
  HRESULT shift_end{S_OK};
  HRESULT set_text{S_OK};
  HRESULT get_text{S_OK};
  // A provider that refuses to move without reporting a failure. PlaceCaret
  // must treat the short shift as an error instead of leaving the caret
  // outside the pair it just inserted.
  bool shift_start_reports_zero{false};
  bool get_text_reports_zero{false};
};

// Offsets are UTF-16 code units, matching the ACP offsets msctf hands a TIP.
struct FakeDocument {
  std::wstring text;
  LONG selection_start{0};
  LONG selection_end{0};
  RangeFaults faults;
  std::vector<std::wstring> set_text_calls;
  int set_selection_count{0};
  bool last_selection_collapsed{false};
};

class FakeRange final : public ITfRange {
 public:
  FakeRange(FakeDocument& document, LONG start, LONG end)
      : document_(document), start_(start), end_(end) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid != IID_IUnknown && riid != IID_ITfRange) return E_NOINTERFACE;
    *object = static_cast<ITfRange*>(this);
    AddRef();
    return S_OK;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&references_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    const auto remaining = static_cast<ULONG>(InterlockedDecrement(&references_));
    if (!remaining) delete this;
    return remaining;
  }

  STDMETHODIMP GetText(TfEditCookie, DWORD flags, WCHAR* buffer, ULONG maximum,
                       ULONG* length) override {
    if (!length) return E_POINTER;
    *length = 0;
    if (FAILED(document_.faults.get_text)) return document_.faults.get_text;
    if (document_.faults.get_text_reports_zero) return S_OK;
    const auto available = static_cast<ULONG>(end_ - start_);
    const ULONG copied = (std::min)(available, maximum);
    if (buffer && copied) {
      std::copy_n(document_.text.begin() + static_cast<std::ptrdiff_t>(start_), copied, buffer);
    }
    *length = copied;
    if (flags & TF_TF_MOVESTART) start_ += static_cast<LONG>(copied);
    return S_OK;
  }

  STDMETHODIMP SetText(TfEditCookie, DWORD, const WCHAR* text, LONG length) override {
    if (FAILED(document_.faults.set_text)) return document_.faults.set_text;
    const std::wstring replacement(text ? text : L"",
                                   text ? static_cast<size_t>(length) : size_t{0});
    document_.set_text_calls.push_back(replacement);
    document_.text.replace(static_cast<size_t>(start_), static_cast<size_t>(end_ - start_),
                           replacement);
    // A real range keeps covering the text it just wrote. PlaceCaret relies on it.
    end_ = start_ + static_cast<LONG>(replacement.size());
    return S_OK;
  }

  STDMETHODIMP GetFormattedText(TfEditCookie, IDataObject** data) override {
    if (data) *data = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown** object) override {
    if (object) *object = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP InsertEmbedded(TfEditCookie, DWORD, IDataObject*) override { return E_NOTIMPL; }

  STDMETHODIMP ShiftStart(TfEditCookie, LONG shift, LONG* shifted, const TF_HALTCOND*) override {
    if (shifted) *shifted = 0;
    if (FAILED(document_.faults.shift_start)) return document_.faults.shift_start;
    if (document_.faults.shift_start_reports_zero) return S_OK;
    const LONG target = Clamp(start_ + shift);
    if (shifted) *shifted = target - start_;
    start_ = target;
    end_ = (std::max)(start_, end_);
    return S_OK;
  }

  STDMETHODIMP ShiftEnd(TfEditCookie, LONG shift, LONG* shifted, const TF_HALTCOND*) override {
    if (shifted) *shifted = 0;
    if (FAILED(document_.faults.shift_end)) return document_.faults.shift_end;
    const LONG target = Clamp(end_ + shift);
    if (shifted) *shifted = target - end_;
    end_ = target;
    start_ = (std::min)(start_, end_);
    return S_OK;
  }

  STDMETHODIMP ShiftStartToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }

  STDMETHODIMP ShiftEndToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }

  STDMETHODIMP ShiftStartRegion(TfEditCookie, TfShiftDir, BOOL* no_region) override {
    if (no_region) *no_region = FALSE;
    return E_NOTIMPL;
  }

  STDMETHODIMP ShiftEndRegion(TfEditCookie, TfShiftDir, BOOL* no_region) override {
    if (no_region) *no_region = FALSE;
    return E_NOTIMPL;
  }

  STDMETHODIMP IsEmpty(TfEditCookie, BOOL* empty) override {
    if (!empty) return E_POINTER;
    *empty = start_ == end_ ? TRUE : FALSE;
    return S_OK;
  }

  STDMETHODIMP Collapse(TfEditCookie, TfAnchor anchor) override {
    if (FAILED(document_.faults.collapse)) return document_.faults.collapse;
    if (anchor == TF_ANCHOR_START) {
      end_ = start_;
    } else {
      start_ = end_;
    }
    return S_OK;
  }

  STDMETHODIMP IsEqualStart(TfEditCookie, ITfRange*, TfAnchor, BOOL* equal) override {
    if (equal) *equal = FALSE;
    return E_NOTIMPL;
  }

  STDMETHODIMP IsEqualEnd(TfEditCookie, ITfRange*, TfAnchor, BOOL* equal) override {
    if (equal) *equal = FALSE;
    return E_NOTIMPL;
  }

  STDMETHODIMP CompareStart(TfEditCookie, ITfRange*, TfAnchor, LONG* result) override {
    if (result) *result = 0;
    return E_NOTIMPL;
  }

  STDMETHODIMP CompareEnd(TfEditCookie, ITfRange*, TfAnchor, LONG* result) override {
    if (result) *result = 0;
    return E_NOTIMPL;
  }

  STDMETHODIMP AdjustForInsert(TfEditCookie, ULONG, BOOL* insert_ok) override {
    if (insert_ok) *insert_ok = TRUE;
    return S_OK;
  }

  STDMETHODIMP GetGravity(TfGravity* start, TfGravity* end) override {
    if (!start || !end) return E_POINTER;
    *start = TF_GRAVITY_FORWARD;
    *end = TF_GRAVITY_FORWARD;
    return S_OK;
  }

  STDMETHODIMP SetGravity(TfEditCookie, TfGravity, TfGravity) override { return S_OK; }

  STDMETHODIMP Clone(ITfRange** clone) override {
    if (!clone) return E_POINTER;
    *clone = nullptr;
    if (FAILED(document_.faults.clone)) return document_.faults.clone;
    *clone = static_cast<ITfRange*>(new FakeRange(document_, start_, end_));
    return S_OK;
  }

  STDMETHODIMP GetContext(ITfContext** context) override {
    if (context) *context = nullptr;
    return E_NOTIMPL;
  }

  LONG start() const { return start_; }
  LONG end() const { return end_; }

 private:
  LONG Clamp(LONG offset) const {
    const auto size = static_cast<LONG>(document_.text.size());
    return (std::max)(LONG{0}, (std::min)(offset, size));
  }

  FakeDocument& document_;
  LONG start_{0};
  LONG end_{0};
  LONG references_{1};
};

struct FakeRangeReleaser {
  void operator()(FakeRange* range) const { range->Release(); }
};

class FakeComposition final : public ITfComposition {
 public:
  FakeComposition(FakeDocument& document, LONG start, LONG end)
      : range_(new FakeRange(document, start, end)) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid != IID_IUnknown && riid != IID_ITfComposition) return E_NOINTERFACE;
    *object = static_cast<ITfComposition*>(this);
    AddRef();
    return S_OK;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&references_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    const auto remaining = static_cast<ULONG>(InterlockedDecrement(&references_));
    if (!remaining) delete this;
    return remaining;
  }

  STDMETHODIMP GetRange(ITfRange** range) override {
    if (!range) return E_POINTER;
    *range = nullptr;
    if (FAILED(get_range_result)) return get_range_result;
    range_->AddRef();
    *range = range_.get();
    return S_OK;
  }

  STDMETHODIMP ShiftStart(TfEditCookie, ITfRange*) override { return E_NOTIMPL; }

  STDMETHODIMP ShiftEnd(TfEditCookie, ITfRange*) override { return E_NOTIMPL; }

  STDMETHODIMP EndComposition(TfEditCookie) override {
    ++end_count;
    return S_OK;
  }

  int end_count{0};
  HRESULT get_range_result{S_OK};

 private:
  std::unique_ptr<FakeRange, FakeRangeReleaser> range_;
  LONG references_{1};
};

enum class SessionMode {
  kRunSynchronously,
  kRequestFails,
  kAcceptsWithoutRunning,
  kRetainsForLater,
};

class FakeContext final : public ITfContext, public ITfContextComposition {
 public:
  explicit FakeContext(FakeDocument& document) : document_(document) {}

  ~FakeContext() {
    if (retained_session) retained_session->Release();
    if (composition) composition->Release();
  }

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfContext) {
      *object = static_cast<ITfContext*>(this);
      AddRef();
      return S_OK;
    }
    if (riid == IID_ITfContextComposition) {
      if (FAILED(context_composition_result)) return context_composition_result;
      *object = static_cast<ITfContextComposition*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&references_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    return static_cast<ULONG>(InterlockedDecrement(&references_));
  }

  STDMETHODIMP RequestEditSession(TfClientId client_id, ITfEditSession* session, DWORD flags,
                                  HRESULT* session_result) override {
    ++request_count;
    last_client_id = client_id;
    last_flags = flags;
    if (!session_result) return E_POINTER;
    *session_result = S_OK;
    switch (mode) {
      case SessionMode::kRunSynchronously:
        *session_result = session ? session->DoEditSession(edit_cookie) : E_POINTER;
        return S_OK;
      case SessionMode::kRequestFails:
        return E_FAIL;
      case SessionMode::kAcceptsWithoutRunning:
        return S_OK;
      case SessionMode::kRetainsForLater:
        // A provider that wrongly defers a TF_ES_SYNC request keeps the session
        // alive past the call; RunSync must still refuse to report success.
        if (session && !retained_session) {
          session->AddRef();
          retained_session = session;
        }
        return S_OK;
    }
    return S_OK;
  }

  STDMETHODIMP InWriteSession(TfClientId, BOOL* write_session) override {
    if (!write_session) return E_POINTER;
    *write_session = FALSE;
    return S_OK;
  }

  STDMETHODIMP GetSelection(TfEditCookie, ULONG, ULONG count, TF_SELECTION* selection,
                            ULONG* fetched) override {
    if (!fetched) return E_POINTER;
    *fetched = 0;
    if (selection && count) selection[0] = {};
    if (FAILED(get_selection_result)) return get_selection_result;
    if (!selection || !count) return E_INVALIDARG;
    selection[0].range = static_cast<ITfRange*>(
        new FakeRange(document_, document_.selection_start, document_.selection_end));
    selection[0].style.ase = TF_AE_NONE;
    *fetched = 1;
    return S_OK;
  }

  STDMETHODIMP SetSelection(TfEditCookie, ULONG count, const TF_SELECTION* selection) override {
    if (FAILED(set_selection_result)) return set_selection_result;
    if (!selection || !count) return E_INVALIDARG;
    auto* range = static_cast<FakeRange*>(selection[0].range);
    ++document_.set_selection_count;
    document_.selection_start = range->start();
    document_.selection_end = range->end();
    document_.last_selection_collapsed = range->start() == range->end();
    return S_OK;
  }

  STDMETHODIMP GetStart(TfEditCookie, ITfRange** start) override {
    if (start) *start = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetEnd(TfEditCookie, ITfRange** end) override {
    if (end) *end = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetActiveView(ITfContextView** view) override {
    if (view) *view = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP EnumViews(IEnumTfContextViews** views) override {
    if (views) *views = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetStatus(TF_STATUS* status) override {
    if (!status) return E_POINTER;
    *status = {};
    return S_OK;
  }

  STDMETHODIMP GetProperty(REFGUID, ITfProperty** property) override {
    if (property) *property = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetAppProperty(REFGUID, ITfReadOnlyProperty** property) override {
    if (property) *property = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP TrackProperties(const GUID**, ULONG, const GUID**, ULONG,
                               ITfReadOnlyProperty** property) override {
    if (property) *property = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP EnumProperties(IEnumTfProperties** properties) override {
    if (properties) *properties = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr** document_mgr) override {
    if (document_mgr) *document_mgr = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP CreateRangeBackup(TfEditCookie, ITfRange*, ITfRangeBackup** backup) override {
    if (backup) *backup = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP StartComposition(TfEditCookie, ITfRange* composition_range, ITfCompositionSink*,
                                ITfComposition** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    ++start_composition_count;
    if (FAILED(start_composition_result)) return start_composition_result;
    auto* range = static_cast<FakeRange*>(composition_range);
    if (composition) composition->Release();
    composition = new FakeComposition(document_, range->start(), range->end());
    composition->get_range_result = composition_get_range_result;
    composition->AddRef();
    *result = composition;
    return S_OK;
  }

  STDMETHODIMP EnumCompositions(IEnumITfCompositionView** views) override {
    if (views) *views = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP FindComposition(TfEditCookie, ITfRange*, IEnumITfCompositionView** views) override {
    if (views) *views = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP TakeOwnership(TfEditCookie, ITfCompositionView*, ITfCompositionSink*,
                             ITfComposition** result) override {
    if (result) *result = nullptr;
    return E_NOTIMPL;
  }

  SessionMode mode{SessionMode::kRunSynchronously};
  TfEditCookie edit_cookie{1};
  int request_count{0};
  TfClientId last_client_id{TF_CLIENTID_NULL};
  DWORD last_flags{0};
  HRESULT get_selection_result{S_OK};
  HRESULT set_selection_result{S_OK};
  HRESULT context_composition_result{S_OK};
  HRESULT start_composition_result{S_OK};
  HRESULT composition_get_range_result{S_OK};
  int start_composition_count{0};
  ITfEditSession* retained_session{nullptr};
  FakeComposition* composition{nullptr};

 private:
  FakeDocument& document_;
  LONG references_{1};
};

BracketSettings ImmediateSettings() {
  BracketSettings settings;
  settings.pairing.enabled = true;
  settings.trigger = BracketPairingTrigger::Immediate;
  return settings;
}

BracketPairingAction MakeAction(BracketPairingActionType type, char32_t open, char32_t close) {
  BracketPairingAction action;
  action.type = type;
  action.open = open;
  action.close = close;
  return action;
}

// Builds a document with a collapsed caret at `offset`.
FakeDocument MakeDocument(std::wstring text, LONG offset) {
  FakeDocument document;
  document.text = std::move(text);
  document.selection_start = offset;
  document.selection_end = offset;
  return document;
}

constexpr TfClientId kClientId = 7;
constexpr char32_t kOpen = U'「';   // 「
constexpr char32_t kClose = U'」';  // 」

// --- ReadHint -------------------------------------------------------------

TEST(BracketEditSessionReadHint, CollapsedSelectionReadsBothNeighbours) {
  FakeDocument document = MakeDocument(L"あい", 1);
  FakeContext context(document);

  const auto hint = BracketEditSession::ReadHint(&context, kClientId);

  ASSERT_TRUE(hint.selection_collapsed.has_value());
  EXPECT_TRUE(*hint.selection_collapsed);
  ASSERT_TRUE(hint.char_before.has_value());
  EXPECT_EQ(*hint.char_before, U'あ');
  ASSERT_TRUE(hint.char_after.has_value());
  EXPECT_EQ(*hint.char_after, U'い');
  // The hint is read under a synchronous read-only lock (spec 5.3).
  EXPECT_EQ(context.last_flags & TF_ES_SYNC, static_cast<DWORD>(TF_ES_SYNC));
  EXPECT_EQ(context.last_flags & TF_ES_READWRITE, static_cast<DWORD>(TF_ES_READ));
  EXPECT_EQ(context.last_client_id, kClientId);
}

TEST(BracketEditSessionReadHint, DocumentEdgeLeavesTheMissingNeighbourUnknown) {
  FakeDocument document = MakeDocument(L"あ", 0);
  FakeContext context(document);

  const auto hint = BracketEditSession::ReadHint(&context, kClientId);

  ASSERT_TRUE(hint.selection_collapsed.has_value());
  EXPECT_TRUE(*hint.selection_collapsed);
  EXPECT_FALSE(hint.char_before.has_value());
  ASSERT_TRUE(hint.char_after.has_value());
  EXPECT_EQ(*hint.char_after, U'あ');
}

TEST(BracketEditSessionReadHint, NonCollapsedSelectionReportsNoNeighbours) {
  FakeDocument document = MakeDocument(L"あい", 0);
  document.selection_end = 2;
  FakeContext context(document);

  const auto hint = BracketEditSession::ReadHint(&context, kClientId);

  ASSERT_TRUE(hint.selection_collapsed.has_value());
  EXPECT_FALSE(*hint.selection_collapsed);
  EXPECT_FALSE(hint.char_before.has_value());
  EXPECT_FALSE(hint.char_after.has_value());
}

TEST(BracketEditSessionReadHint, FailedSelectionYieldsAnUnknownHint) {
  FakeDocument document = MakeDocument(L"あい", 1);
  FakeContext context(document);
  context.get_selection_result = E_FAIL;

  const auto hint = BracketEditSession::ReadHint(&context, kClientId);

  // Unknown, not "empty document": an empty hint must never authorize a delete.
  EXPECT_FALSE(hint.selection_collapsed.has_value());
  EXPECT_FALSE(hint.char_before.has_value());
  EXPECT_FALSE(hint.char_after.has_value());
}

TEST(BracketEditSessionReadHint, ShortNeighbourReadYieldsAnUnknownHint) {
  FakeDocument document = MakeDocument(L"あい", 1);
  document.faults.get_text_reports_zero = true;
  FakeContext context(document);

  const auto hint = BracketEditSession::ReadHint(&context, kClientId);

  EXPECT_FALSE(hint.selection_collapsed.has_value());
}

TEST(BracketEditSessionReadHint, FailedNeighbourShiftYieldsAnUnknownHint) {
  FakeDocument document = MakeDocument(L"あい", 1);
  document.faults.shift_start = E_FAIL;
  FakeContext context(document);

  const auto hint = BracketEditSession::ReadHint(&context, kClientId);

  EXPECT_FALSE(hint.selection_collapsed.has_value());
}

TEST(BracketEditSessionReadHint, FailedNeighbourReadYieldsAnUnknownHint) {
  FakeDocument document = MakeDocument(L"あい", 1);
  document.faults.get_text = E_FAIL;
  FakeContext context(document);

  const auto hint = BracketEditSession::ReadHint(&context, kClientId);

  EXPECT_FALSE(hint.selection_collapsed.has_value());
}

TEST(BracketEditSessionReadHint, FailedForwardNeighbourShiftYieldsAnUnknownHint) {
  FakeDocument document = MakeDocument(L"あい", 1);
  document.faults.shift_end = E_FAIL;
  FakeContext context(document);

  const auto hint = BracketEditSession::ReadHint(&context, kClientId);

  EXPECT_FALSE(hint.selection_collapsed.has_value());
}

TEST(BracketEditSessionReadHint, RefusedSessionYieldsAnUnknownHint) {
  FakeDocument document = MakeDocument(L"あい", 1);
  FakeContext context(document);
  context.mode = SessionMode::kRequestFails;

  const auto hint = BracketEditSession::ReadHint(&context, kClientId);

  EXPECT_FALSE(hint.selection_collapsed.has_value());
}

// --- RunSync --------------------------------------------------------------

TEST(BracketEditSessionRunSync, MissingContextIsRejected) {
  azookey::tsf::TextService service;
  bool applied = true;

  const HRESULT hr = BracketEditSession::Apply(
      service, nullptr, kClientId, MakeAction(BracketPairingActionType::kInsertPair, kOpen, kClose),
      ImmediateSettings(), applied);

  EXPECT_EQ(hr, E_INVALIDARG);
  EXPECT_FALSE(applied);
}

TEST(BracketEditSessionRunSync, AcceptedButUnexecutedSessionFails) {
  FakeDocument document = MakeDocument(L"", 0);
  FakeContext context(document);
  context.mode = SessionMode::kAcceptsWithoutRunning;
  azookey::tsf::TextService service;
  bool applied = true;

  const HRESULT hr = BracketEditSession::Apply(
      service, &context, kClientId,
      MakeAction(BracketPairingActionType::kInsertPair, kOpen, kClose), ImmediateSettings(),
      applied);

  EXPECT_EQ(hr, E_FAIL);
  EXPECT_FALSE(applied);
  EXPECT_TRUE(document.text.empty());
}

TEST(BracketEditSessionRunSync, RetainedSessionCannotRunAfterTheRequestReturns) {
  FakeDocument document = MakeDocument(L"", 0);
  FakeContext context(document);
  context.mode = SessionMode::kRetainsForLater;
  azookey::tsf::TextService service;
  bool applied = true;

  const HRESULT hr = BracketEditSession::Apply(
      service, &context, kClientId,
      MakeAction(BracketPairingActionType::kInsertPair, kOpen, kClose), ImmediateSettings(),
      applied);

  EXPECT_EQ(hr, E_FAIL);
  EXPECT_FALSE(applied);
  ASSERT_NE(context.retained_session, nullptr);
  // Disarm() ran before the request returned, so the deferred callback is inert
  // and cannot touch the stack captures it was built from.
  EXPECT_EQ(context.retained_session->DoEditSession(context.edit_cookie), E_UNEXPECTED);
  EXPECT_TRUE(document.text.empty());
}

// --- Apply: immediate insertion and caret failure after the write landed ---
// These fix the TSF operation sequence and the post-write failure contract.
// They do not reproduce DEV-1142, whose cause is still undetermined.

TEST(BracketEditSessionApply, ImmediateInsertPairPutsTheCaretBetweenOpenAndClose) {
  FakeDocument document = MakeDocument(L"", 0);
  FakeContext context(document);
  azookey::tsf::TextService service;
  bool applied = false;

  const HRESULT hr = BracketEditSession::Apply(
      service, &context, kClientId,
      MakeAction(BracketPairingActionType::kInsertPair, kOpen, kClose), ImmediateSettings(),
      applied);

  EXPECT_EQ(hr, S_OK);
  EXPECT_TRUE(applied);
  EXPECT_EQ(document.text, L"「」");
  // docs/bracket-pairing-spec.md 5.2: a single SetText, then one collapsed
  // caret between the pair. Leaving the selection non-collapsed would let the
  // next key replace the closing bracket instead of typing inside the pair.
  ASSERT_EQ(document.set_text_calls.size(), 1u);
  EXPECT_EQ(document.set_text_calls.front(), L"「」");
  EXPECT_EQ(document.set_selection_count, 1);
  EXPECT_TRUE(document.last_selection_collapsed);
  EXPECT_EQ(document.selection_start, 1);
  EXPECT_FALSE(service.bracket_composition_for_test());
  EXPECT_EQ(context.last_flags & TF_ES_SYNC, static_cast<DWORD>(TF_ES_SYNC));
  EXPECT_EQ(context.last_flags & TF_ES_READWRITE, static_cast<DWORD>(TF_ES_READWRITE));
}


TEST(BracketEditSessionApply, InsertedPairSurvivesAShortCaretShift) {
  FakeDocument document = MakeDocument(L"", 0);
  document.faults.shift_start_reports_zero = true;
  FakeContext context(document);
  azookey::tsf::TextService service;
  bool applied = false;

  const HRESULT hr = BracketEditSession::Apply(
      service, &context, kClientId,
      MakeAction(BracketPairingActionType::kInsertPair, kOpen, kClose), ImmediateSettings(),
      applied);

  EXPECT_EQ(hr, E_FAIL);
  // The text is already in the document, so `applied` stays true and the caller
  // reports bracket_caret_failed rather than replaying the insertion.
  EXPECT_TRUE(applied);
  EXPECT_EQ(document.text, L"「」");
  EXPECT_EQ(document.set_text_calls.size(), 1u);
  // A refused shift must not leave the caret selecting the closing bracket.
  EXPECT_TRUE(document.set_selection_count == 0 || document.last_selection_collapsed);
}

TEST(BracketEditSessionApply, InsertedPairSurvivesASetSelectionFailure) {
  FakeDocument document = MakeDocument(L"", 0);
  FakeContext context(document);
  context.set_selection_result = E_FAIL;
  azookey::tsf::TextService service;
  bool applied = false;

  EXPECT_EQ(BracketEditSession::Apply(
                service, &context, kClientId,
                MakeAction(BracketPairingActionType::kInsertPair, kOpen, kClose),
                ImmediateSettings(), applied),
            E_FAIL);
  EXPECT_TRUE(applied);
  EXPECT_EQ(document.text, L"「」");
  EXPECT_EQ(document.set_text_calls.size(), 1u);
}

// A document-wide Clone or Collapse failure also breaks the hint read, so the
// pair degrades to a literal insertion. The write still lands, and the caret
// failure that follows must propagate without retracting it.
TEST(BracketEditSessionApply, InsertedLiteralSurvivesACaretCloneFailure) {
  FakeDocument document = MakeDocument(L"", 0);
  document.faults.clone = E_OUTOFMEMORY;
  FakeContext context(document);
  azookey::tsf::TextService service;
  bool applied = false;

  EXPECT_EQ(BracketEditSession::Apply(
                service, &context, kClientId,
                MakeAction(BracketPairingActionType::kInsertPair, kOpen, kClose),
                ImmediateSettings(), applied),
            E_OUTOFMEMORY);
  EXPECT_TRUE(applied);
  EXPECT_EQ(document.text, L"「");
  EXPECT_TRUE(document.set_selection_count == 0 || document.last_selection_collapsed);
}

TEST(BracketEditSessionApply, InsertedLiteralSurvivesACaretCollapseFailure) {
  FakeDocument document = MakeDocument(L"", 0);
  document.faults.collapse = E_FAIL;
  FakeContext context(document);
  azookey::tsf::TextService service;
  bool applied = false;

  EXPECT_EQ(BracketEditSession::Apply(
                service, &context, kClientId,
                MakeAction(BracketPairingActionType::kInsertPair, kOpen, kClose),
                ImmediateSettings(), applied),
            E_FAIL);
  EXPECT_TRUE(applied);
  EXPECT_EQ(document.text, L"「");
  EXPECT_TRUE(document.set_selection_count == 0 || document.last_selection_collapsed);
}

TEST(BracketEditSessionApply, FailedInsertionIsNotReportedAsApplied) {
  FakeDocument document = MakeDocument(L"", 0);
  document.faults.set_text = E_FAIL;
  FakeContext context(document);
  azookey::tsf::TextService service;
  bool applied = true;

  EXPECT_EQ(BracketEditSession::Apply(
                service, &context, kClientId,
                MakeAction(BracketPairingActionType::kInsertPair, kOpen, kClose),
                ImmediateSettings(), applied),
            E_FAIL);
  EXPECT_FALSE(applied);
  EXPECT_TRUE(document.text.empty());
}

// --- Apply: revalidation under the write lock -----------------------------




TEST(BracketEditSessionApply, SkipClosingCaretFailureIsNotReportedAsApplied) {
  FakeDocument document = MakeDocument(L"」", 0);
  FakeContext context(document);
  context.set_selection_result = E_FAIL;
  azookey::tsf::TextService service;
  bool applied = true;

  // Unlike insertion, skipping writes nothing, so a caret failure leaves the
  // document untouched and nothing to protect from a replay.
  EXPECT_EQ(BracketEditSession::Apply(
                service, &context, kClientId,
                MakeAction(BracketPairingActionType::kSkipClosing, kClose, kClose),
                ImmediateSettings(), applied),
            E_FAIL);
  EXPECT_FALSE(applied);
  EXPECT_EQ(document.text, L"」");
  EXPECT_TRUE(document.set_text_calls.empty());
}



TEST(BracketEditSessionApply, PassThroughWritesNothing) {
  FakeDocument document = MakeDocument(L"ab", 1);
  FakeContext context(document);
  azookey::tsf::TextService service;
  bool applied = true;

  EXPECT_EQ(BracketEditSession::Apply(
                service, &context, kClientId,
                MakeAction(BracketPairingActionType::kPassThrough, kOpen, kClose),
                ImmediateSettings(), applied),
            S_FALSE);
  EXPECT_FALSE(applied);
  EXPECT_EQ(document.text, L"ab");
  EXPECT_TRUE(document.set_text_calls.empty());
}




// --- Apply + Finish: composition trigger ----------------------------------


TEST(BracketEditSessionComposition, CancelRemovesThePair) {
  FakeDocument document = MakeDocument(L"a", 1);
  FakeContext context(document);
  BracketSettings settings = ImmediateSettings();
  settings.trigger = BracketPairingTrigger::Composition;
  azookey::tsf::TextService service;
  bool applied = false;

  ASSERT_EQ(BracketEditSession::Apply(
                service, &context, kClientId,
                MakeAction(BracketPairingActionType::kInsertPair, kOpen, kClose), settings,
                applied),
            S_OK);
  ASSERT_TRUE(service.bracket_composition_for_test());

  ASSERT_EQ(BracketEditSession::Finish(service, &context, kClientId, /*cancel=*/true), S_OK);

  EXPECT_FALSE(service.bracket_composition_for_test());
  EXPECT_EQ(document.text, L"a");
  EXPECT_EQ(document.selection_start, 1);
}

TEST(BracketEditSessionComposition, MissingCompositionSupportFailsWithoutWriting) {
  FakeDocument document = MakeDocument(L"", 0);
  FakeContext context(document);
  context.context_composition_result = E_NOINTERFACE;
  BracketSettings settings = ImmediateSettings();
  settings.trigger = BracketPairingTrigger::Composition;
  azookey::tsf::TextService service;
  bool applied = true;

  EXPECT_EQ(BracketEditSession::Apply(
                service, &context, kClientId,
                MakeAction(BracketPairingActionType::kInsertPair, kOpen, kClose), settings,
                applied),
            E_NOINTERFACE);
  EXPECT_FALSE(applied);
  EXPECT_TRUE(document.text.empty());
  EXPECT_FALSE(service.bracket_composition_for_test());
}

TEST(BracketEditSessionComposition, RefusedCompositionStartLeavesNoText) {
  FakeDocument document = MakeDocument(L"", 0);
  FakeContext context(document);
  context.start_composition_result = E_FAIL;
  BracketSettings settings = ImmediateSettings();
  settings.trigger = BracketPairingTrigger::Composition;
  azookey::tsf::TextService service;
  bool applied = true;

  EXPECT_EQ(BracketEditSession::Apply(
                service, &context, kClientId,
                MakeAction(BracketPairingActionType::kInsertPair, kOpen, kClose), settings,
                applied),
            E_FAIL);
  EXPECT_FALSE(applied);
  EXPECT_TRUE(document.text.empty());
  EXPECT_FALSE(service.bracket_composition_for_test());
}

TEST(BracketEditSessionComposition, UnreadableCompositionRangeEndsTheComposition) {
  FakeDocument document = MakeDocument(L"", 0);
  FakeContext context(document);
  context.composition_get_range_result = E_FAIL;
  BracketSettings settings = ImmediateSettings();
  settings.trigger = BracketPairingTrigger::Composition;
  azookey::tsf::TextService service;
  bool applied = true;

  EXPECT_EQ(BracketEditSession::Apply(
                service, &context, kClientId,
                MakeAction(BracketPairingActionType::kInsertPair, kOpen, kClose), settings,
                applied),
            E_FAIL);
  EXPECT_FALSE(applied);
  EXPECT_TRUE(document.text.empty());
  EXPECT_FALSE(service.bracket_composition_for_test());
  // The composition was started, so the failure path has to close it.
  ASSERT_NE(context.composition, nullptr);
  EXPECT_EQ(context.composition->end_count, 1);
}

TEST(BracketEditSessionComposition, FinishWithoutACompositionIsANoOp) {
  FakeDocument document = MakeDocument(L"ab", 1);
  FakeContext context(document);
  azookey::tsf::TextService service;

  EXPECT_EQ(BracketEditSession::Finish(service, &context, kClientId, /*cancel=*/false), S_OK);
  EXPECT_EQ(context.request_count, 0);
  EXPECT_EQ(document.text, L"ab");
}

}  // namespace
