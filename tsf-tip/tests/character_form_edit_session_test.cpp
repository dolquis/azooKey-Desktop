#include <Windows.h>
#include <gtest/gtest.h>
#include <msctf.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "azookey/tsf/CharacterFormEditSession.h"

namespace {

using azookey::tsf::CharacterFormCycleState;
using azookey::tsf::CharacterFormEditSession;

struct Document {
  std::wstring text{L"あした"};
  LONG start{0};
  LONG end{3};
  bool short_read{false};
  bool refuse_read{false};
  bool refuse_insert{false};
  bool refuse_selection{false};
  bool no_progress{false};
  int inserts{0};
  int selection_sets{0};
  int fail_selection_call{0};
};

class Range final : public ITfRange {
 public:
  Range(Document& document, LONG start, LONG end) : document_(document), start_(start), end_(end) {}
  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfRange) return E_NOINTERFACE;
    *object = static_cast<ITfRange*>(this);
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
  STDMETHODIMP GetText(TfEditCookie, DWORD flags, WCHAR* text, ULONG maximum,
                       ULONG* length) override {
    if (!length) return E_POINTER;
    *length = 0;
    if (document_.refuse_read) return E_FAIL;
    if (document_.no_progress) return S_OK;
    ULONG count = (std::min)(maximum, static_cast<ULONG>(end_ - start_));
    if (document_.short_read) count = (std::min)(count, ULONG{1});
    if (count && text) std::copy_n(document_.text.data() + start_, count, text);
    *length = count;
    if (flags & TF_TF_MOVESTART) start_ += static_cast<LONG>(count);
    return S_OK;
  }
  STDMETHODIMP SetText(TfEditCookie, DWORD, const WCHAR*, LONG) override { return E_NOTIMPL; }
  STDMETHODIMP GetFormattedText(TfEditCookie, IDataObject** object) override {
    if (object) *object = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown** object) override {
    if (object) *object = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP InsertEmbedded(TfEditCookie, DWORD, IDataObject*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStart(TfEditCookie, LONG requested, LONG* shifted,
                          const TF_HALTCOND*) override {
    const LONG target = (std::max)(
        LONG{0}, (std::min)(start_ + requested, static_cast<LONG>(document_.text.size())));
    if (shifted) *shifted = target - start_;
    start_ = target;
    end_ = (std::max)(end_, start_);
    return S_OK;
  }
  STDMETHODIMP ShiftEnd(TfEditCookie, LONG, LONG* shifted, const TF_HALTCOND*) override {
    if (shifted) *shifted = 0;
    return E_NOTIMPL;
  }
  STDMETHODIMP ShiftStartToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftEndToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStartRegion(TfEditCookie, TfShiftDir, BOOL* no_region) override {
    if (no_region) *no_region = TRUE;
    return E_NOTIMPL;
  }
  STDMETHODIMP ShiftEndRegion(TfEditCookie, TfShiftDir, BOOL* no_region) override {
    if (no_region) *no_region = TRUE;
    return E_NOTIMPL;
  }
  STDMETHODIMP IsEmpty(TfEditCookie, BOOL* empty) override {
    if (!empty) return E_POINTER;
    *empty = start_ == end_ ? TRUE : FALSE;
    return S_OK;
  }
  STDMETHODIMP Collapse(TfEditCookie, TfAnchor anchor) override {
    if (anchor == TF_ANCHOR_START)
      end_ = start_;
    else
      start_ = end_;
    return S_OK;
  }
  STDMETHODIMP IsEqualStart(TfEditCookie, ITfRange* other, TfAnchor anchor, BOOL* equal) override {
    if (!other || !equal) return E_POINTER;
    const auto* range = static_cast<Range*>(other);
    *equal = start_ == (anchor == TF_ANCHOR_START ? range->start_ : range->end_) ? TRUE : FALSE;
    return S_OK;
  }
  STDMETHODIMP IsEqualEnd(TfEditCookie, ITfRange*, TfAnchor, BOOL* equal) override {
    if (equal) *equal = FALSE;
    return E_NOTIMPL;
  }
  STDMETHODIMP CompareStart(TfEditCookie, ITfRange*, TfAnchor, LONG* compared) override {
    if (compared) *compared = 0;
    return E_NOTIMPL;
  }
  STDMETHODIMP CompareEnd(TfEditCookie, ITfRange*, TfAnchor, LONG* compared) override {
    if (compared) *compared = 0;
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
    *clone = new Range(document_, start_, end_);
    return S_OK;
  }
  STDMETHODIMP GetContext(ITfContext** context) override {
    if (context) *context = nullptr;
    return E_NOTIMPL;
  }
  LONG start() const { return start_; }
  LONG end() const { return end_; }

 private:
  Document& document_;
  LONG start_;
  LONG end_;
  LONG references_{1};
};

class Context final : public ITfContext, public ITfInsertAtSelection {
 public:
  explicit Context(Document& document) : document_(document) {}
  ~Context() {
    if (retained_session) retained_session->Release();
  }
  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_ITfContext)
      *object = static_cast<ITfContext*>(this);
    else if (iid == IID_ITfInsertAtSelection)
      *object = static_cast<ITfInsertAtSelection*>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&references_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    return static_cast<ULONG>(InterlockedDecrement(&references_));
  }
  STDMETHODIMP RequestEditSession(TfClientId id, ITfEditSession* session, DWORD flags,
                                  HRESULT* result) override {
    ++requests;
    last_id = id;
    last_flags = flags;
    if (!result) return E_POINTER;
    *result = E_FAIL;
    if (refuse_session) return S_OK;
    if (defer_session) {
      retained_session = session;
      retained_session->AddRef();
      return S_OK;
    }
    *result = session->DoEditSession(1);
    return S_OK;
  }
  STDMETHODIMP InWriteSession(TfClientId, BOOL* write) override {
    if (!write) return E_POINTER;
    *write = TRUE;
    return S_OK;
  }
  STDMETHODIMP GetSelection(TfEditCookie, ULONG, ULONG count, TF_SELECTION* selection,
                            ULONG* fetched) override {
    if (!selection || !fetched || count != 1) return E_INVALIDARG;
    *selection = {};
    *fetched = 1;
    selection->range = new Range(document_, document_.start, document_.end);
    selection->style.ase = TF_AE_END;
    return S_OK;
  }
  STDMETHODIMP SetSelection(TfEditCookie, ULONG count, const TF_SELECTION* selection) override {
    if (!selection || count != 1) return E_INVALIDARG;
    ++selection_calls;
    if (document_.fail_selection_call == selection_calls) return E_FAIL;
    if (document_.refuse_selection) return E_FAIL;
    auto* range = static_cast<Range*>(selection->range);
    document_.start = range->start();
    document_.end = range->end();
    ++document_.selection_sets;
    return S_OK;
  }
  STDMETHODIMP InsertTextAtSelection(TfEditCookie, DWORD flags, const WCHAR* text, LONG length,
                                     ITfRange** inserted) override {
    if (!inserted || !text || length < 0 || flags) return E_INVALIDARG;
    *inserted = nullptr;
    if (document_.refuse_insert) return TS_E_READONLY;
    const LONG start = document_.start;
    document_.text.replace(static_cast<size_t>(start), static_cast<size_t>(document_.end - start),
                           text, static_cast<size_t>(length));
    document_.end = start + length;
    ++document_.inserts;
    *inserted = new Range(document_, start, document_.end);
    return S_OK;
  }
  STDMETHODIMP InsertEmbeddedAtSelection(TfEditCookie, DWORD, IDataObject*,
                                         ITfRange** range) override {
    if (range) *range = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetStart(TfEditCookie, ITfRange** range) override {
    if (range) *range = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetEnd(TfEditCookie, ITfRange** range) override {
    if (range) *range = nullptr;
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
  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr** manager) override {
    if (manager) *manager = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP CreateRangeBackup(TfEditCookie, ITfRange*, ITfRangeBackup** backup) override {
    if (backup) *backup = nullptr;
    return E_NOTIMPL;
  }

  bool refuse_session{false};
  bool defer_session{false};
  ITfEditSession* retained_session{nullptr};
  int requests{0};
  int selection_calls{0};
  TfClientId last_id{TF_CLIENTID_NULL};
  DWORD last_flags{0};

 private:
  Document& document_;
  LONG references_{1};
};

TEST(CharacterFormEditSession, CyclesSelectedTextAndKeepsItSelected) {
  Document doc;
  Context context(doc);
  std::optional<CharacterFormCycleState> state;
  for (const wchar_t* expected : {L"アシタ", L"ｱｼﾀ", L"ＡＳＨＩＴＡ", L"ASHITA", L"あした"}) {
    auto result = CharacterFormEditSession::CycleSelection(&context, 7, state);
    ASSERT_TRUE(result.applied);
    ASSERT_EQ(result.status, S_OK);
    ASSERT_TRUE(result.state);
    EXPECT_EQ(doc.text, expected);
    EXPECT_EQ(doc.start, 0);
    EXPECT_EQ(doc.end, static_cast<LONG>(doc.text.size()));
    state = std::move(result.state);
  }
  EXPECT_EQ(doc.inserts, 5);
  EXPECT_EQ(doc.selection_sets, 5);
  EXPECT_EQ(context.last_id, 7U);
  EXPECT_EQ(context.last_flags, TF_ES_SYNC | TF_ES_READWRITE);
}

TEST(CharacterFormEditSession, ReadOnlyProbeClaimsOnlySupportedSelection) {
  Document doc;
  Context context(doc);
  EXPECT_TRUE(CharacterFormEditSession::CanCycleSelection(&context, 7, std::nullopt));
  EXPECT_EQ(context.last_flags, TF_ES_SYNC | TF_ES_READ);
  EXPECT_EQ(doc.inserts, 0);
  doc.end = 0;
  EXPECT_FALSE(CharacterFormEditSession::CanCycleSelection(&context, 7, std::nullopt));
  doc.text = L"漢字";
  doc.end = 2;
  EXPECT_FALSE(CharacterFormEditSession::CanCycleSelection(&context, 7, std::nullopt));
  EXPECT_EQ(doc.inserts, 0);
}

TEST(CharacterFormEditSession, HiraganaCommitStartsWithKatakana) {
  Document doc;
  doc.start = doc.end = 3;
  Context context(doc);
  auto recent = CharacterFormEditSession::CaptureRecentCommit(&context, 7, "あした", "あした");
  ASSERT_TRUE(recent);
  auto result = CharacterFormEditSession::CycleRecentCommit(&context, 7, *recent);
  ASSERT_TRUE(result.replaced);
  EXPECT_EQ(doc.text, L"アシタ");
}

TEST(CharacterFormEditSession, PassesThroughWithoutSelectionOrSupportedText) {
  Document doc;
  Context context(doc);
  doc.end = 0;
  auto result = CharacterFormEditSession::CycleSelection(&context, 7, std::nullopt);
  EXPECT_FALSE(result.applied);
  EXPECT_EQ(result.status, S_FALSE);
  doc.text = L"漢字";
  doc.end = 2;
  result = CharacterFormEditSession::CycleSelection(&context, 7, std::nullopt);
  EXPECT_FALSE(result.applied);
  EXPECT_EQ(doc.text, L"漢字");
  EXPECT_EQ(doc.inserts, 0);
}

TEST(CharacterFormEditSession, ChangedSelectionStartsANewCycle) {
  Document doc;
  Context context(doc);
  auto first = CharacterFormEditSession::CycleSelection(&context, 7, std::nullopt);
  ASSERT_TRUE(first.state);
  doc.text = L"いぬ";
  doc.end = 2;
  auto second = CharacterFormEditSession::CycleSelection(&context, 7, first.state);
  ASSERT_TRUE(second.applied);
  EXPECT_EQ(doc.text, L"イヌ");
  ASSERT_TRUE(second.state);
  EXPECT_EQ(second.state->index, azookey::core::CharacterFormCycle::kKatakana);
}

TEST(CharacterFormEditSession, PartialReadsCannotTruncateSelection) {
  Document doc;
  Context context(doc);
  doc.short_read = true;
  auto result = CharacterFormEditSession::CycleSelection(&context, 7, std::nullopt);
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(doc.text, L"アシタ");

  doc.text = L"あした";
  doc.end = 3;
  doc.no_progress = true;
  result = CharacterFormEditSession::CycleSelection(&context, 7, std::nullopt);
  EXPECT_FALSE(result.applied);
  EXPECT_EQ(doc.text, L"あした");
}

TEST(CharacterFormEditSession, OversizedOrUnreadableSelectionIsUntouched) {
  Document doc;
  Context context(doc);
  doc.text = std::wstring(4097, L'あ');
  doc.end = static_cast<LONG>(doc.text.size());
  auto result = CharacterFormEditSession::CycleSelection(&context, 7, std::nullopt);
  EXPECT_FALSE(result.applied);
  EXPECT_EQ(doc.inserts, 0);
  EXPECT_EQ(doc.text.size(), 4097U);

  doc.text = L"あした";
  doc.end = 3;
  doc.refuse_read = true;
  result = CharacterFormEditSession::CycleSelection(&context, 7, std::nullopt);
  EXPECT_FALSE(result.applied);
  EXPECT_EQ(result.status, E_FAIL);
  EXPECT_EQ(doc.text, L"あした");
}

TEST(CharacterFormEditSession, RefusedWriteLeavesOriginalText) {
  Document doc;
  Context context(doc);
  doc.refuse_insert = true;
  auto result = CharacterFormEditSession::CycleSelection(&context, 7, std::nullopt);
  EXPECT_FALSE(result.applied);
  EXPECT_EQ(result.status, TS_E_READONLY);
  EXPECT_EQ(doc.text, L"あした");
  EXPECT_EQ(doc.inserts, 0);
}

TEST(CharacterFormEditSession, RefusedSessionDoesNotClaimToHaveEdited) {
  Document doc;
  Context context(doc);
  context.refuse_session = true;
  auto result = CharacterFormEditSession::CycleSelection(&context, 7, std::nullopt);
  EXPECT_FALSE(result.applied);
  EXPECT_EQ(result.status, E_FAIL);
  EXPECT_EQ(doc.text, L"あした");
}

TEST(CharacterFormEditSession, FailedSelectionAfterWriteStillConsumesKey) {
  Document doc;
  Context context(doc);
  doc.refuse_selection = true;
  auto result = CharacterFormEditSession::CycleSelection(&context, 7, std::nullopt);
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.status, E_FAIL);
  EXPECT_FALSE(result.state);
  EXPECT_EQ(doc.text, L"アシタ");
}

TEST(CharacterFormEditSession, DeferredSyncSessionCannotUseCallerState) {
  Document doc;
  Context context(doc);
  context.defer_session = true;
  auto result = CharacterFormEditSession::CycleSelection(&context, 7, std::nullopt);
  EXPECT_FALSE(result.applied);
  EXPECT_EQ(result.status, E_FAIL);
  EXPECT_EQ(context.retained_session->DoEditSession(1), E_UNEXPECTED);
  EXPECT_EQ(doc.text, L"あした");
}

TEST(CharacterFormEditSession, CyclesRecentCommitWithoutSelection) {
  Document doc;
  doc.text = L"明日";
  doc.start = doc.end = 2;
  Context context(doc);
  auto recent = CharacterFormEditSession::CaptureRecentCommit(&context, 7, "あした", "明日");
  ASSERT_TRUE(recent);
  EXPECT_EQ(context.last_flags, TF_ES_SYNC | TF_ES_READ);
  for (const wchar_t* expected :
       {L"あした", L"アシタ", L"ｱｼﾀ", L"ＡＳＨＩＴＡ", L"ASHITA", L"あした"}) {
    auto result = CharacterFormEditSession::CycleRecentCommit(&context, 7, *recent);
    ASSERT_TRUE(result.consume_key);
    ASSERT_TRUE(result.replaced);
    ASSERT_EQ(result.status, S_OK);
    ASSERT_TRUE(result.state);
    EXPECT_EQ(doc.text, expected);
    EXPECT_EQ(doc.start, static_cast<LONG>(doc.text.size()));
    EXPECT_EQ(doc.end, static_cast<LONG>(doc.text.size()));
    recent = std::move(result.state);
  }
  EXPECT_EQ(doc.inserts, 6);
  EXPECT_EQ(context.last_flags, TF_ES_SYNC | TF_ES_READWRITE);
}

TEST(CharacterFormEditSession, RecentCommitMustStillMatchCaretAndText) {
  Document doc;
  doc.text = L"明日";
  doc.start = doc.end = 2;
  Context context(doc);
  auto recent = CharacterFormEditSession::CaptureRecentCommit(&context, 7, "あした", "明日");
  ASSERT_TRUE(recent);

  doc.start = doc.end = 0;
  auto result = CharacterFormEditSession::CycleRecentCommit(&context, 7, *recent);
  EXPECT_FALSE(result.consume_key);
  EXPECT_FALSE(result.replaced);
  EXPECT_EQ(doc.text, L"明日");

  doc.start = doc.end = 2;
  doc.text = L"休日";
  result = CharacterFormEditSession::CycleRecentCommit(&context, 7, *recent);
  EXPECT_FALSE(result.consume_key);
  EXPECT_FALSE(result.replaced);
  EXPECT_EQ(doc.text, L"休日");
  EXPECT_EQ(doc.inserts, 0);
}

TEST(CharacterFormEditSession, RecentCommitCannotCrossContexts) {
  Document doc;
  doc.text = L"明日";
  doc.start = doc.end = 2;
  Context original(doc);
  Context other(doc);
  auto recent = CharacterFormEditSession::CaptureRecentCommit(&original, 7, "あした", "明日");
  ASSERT_TRUE(recent);
  auto result = CharacterFormEditSession::CycleRecentCommit(&other, 7, *recent);
  EXPECT_FALSE(result.consume_key);
  EXPECT_FALSE(result.replaced);
  EXPECT_EQ(doc.text, L"明日");
}

TEST(CharacterFormEditSession, CaptureRequiresMatchingCompletedCommit) {
  Document doc;
  doc.text = L"明日";
  doc.start = doc.end = 2;
  Context context(doc);
  EXPECT_FALSE(CharacterFormEditSession::CaptureRecentCommit(&context, 7, "あした", "昨日"));
  EXPECT_FALSE(CharacterFormEditSession::CaptureRecentCommit(&context, 7, "漢字", "明日"));
  doc.start = 0;
  EXPECT_FALSE(CharacterFormEditSession::CaptureRecentCommit(&context, 7, "あした", "明日"));
}

TEST(CharacterFormEditSession, RefusedRecentCommitWriteRestoresCaret) {
  Document doc;
  doc.text = L"明日";
  doc.start = doc.end = 2;
  Context context(doc);
  auto recent = CharacterFormEditSession::CaptureRecentCommit(&context, 7, "あした", "明日");
  ASSERT_TRUE(recent);
  doc.refuse_insert = true;
  auto result = CharacterFormEditSession::CycleRecentCommit(&context, 7, *recent);
  EXPECT_FALSE(result.consume_key);
  EXPECT_FALSE(result.replaced);
  EXPECT_EQ(result.status, TS_E_READONLY);
  EXPECT_EQ(doc.text, L"明日");
  EXPECT_EQ(doc.start, 2);
  EXPECT_EQ(doc.end, 2);
}

TEST(CharacterFormEditSession, FailedCaretRestoreConsumesKeyToProtectText) {
  Document doc;
  doc.text = L"明日";
  doc.start = doc.end = 2;
  Context context(doc);
  auto recent = CharacterFormEditSession::CaptureRecentCommit(&context, 7, "あした", "明日");
  ASSERT_TRUE(recent);
  doc.refuse_insert = true;
  doc.fail_selection_call = 2;
  auto result = CharacterFormEditSession::CycleRecentCommit(&context, 7, *recent);
  EXPECT_TRUE(result.consume_key);
  EXPECT_FALSE(result.replaced);
  EXPECT_EQ(doc.text, L"明日");
  EXPECT_EQ(doc.start, 0);
  EXPECT_EQ(doc.end, 2);
}

}  // namespace
