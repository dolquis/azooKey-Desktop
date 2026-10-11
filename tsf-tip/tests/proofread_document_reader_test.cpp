#include <Windows.h>
#include <gtest/gtest.h>
#include <msctf.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "KeyboardStateGuard.h"
#include "azookey/tsf/AiInputGuard.h"
#include "azookey/tsf/ProofreadDocumentReader.h"
#include "azookey/tsf/TextService.h"

// Batch correction view (docs/rich-features-spec.md X-3-6): the window cut to
// the DetectAnomalies limit, the document read, and the hotkey's privacy gate.

namespace {

using azookey::tsf::ProofreadCapture;
using azookey::tsf::ReadProofreadDocument;
using azookey::tsf::SelectProofreadWindow;

struct Document {
  std::wstring text;
  // Most code units one GetText call returns.
  size_t max_chunk{static_cast<size_t>(-1)};
  HRESULT get_text_result{S_OK};
  int get_text_calls{0};
  size_t code_units_read{0};
};

class TestContext;

class TestRange final : public ITfRange {
 public:
  TestRange(TestContext* context, std::shared_ptr<Document> document, size_t start, size_t end)
      : context_(context), document_(std::move(document)), start_(start), end_(end) {}

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfRange) return E_NOINTERFACE;
    *out = static_cast<ITfRange*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG refs = --refs_;
    if (!refs) delete this;
    return refs;
  }
  STDMETHODIMP GetText(TfEditCookie, DWORD flags, WCHAR* out, ULONG max, ULONG* length) override {
    if (!out || !length) return E_POINTER;
    ++document_->get_text_calls;
    *length = 0;
    if (FAILED(document_->get_text_result)) return document_->get_text_result;
    const size_t size = (std::min)({static_cast<size_t>(max), end_ - start_, document_->max_chunk});
    std::copy_n(document_->text.data() + start_, size, out);
    *length = static_cast<ULONG>(size);
    document_->code_units_read += size;
    if (flags & TF_TF_MOVESTART) start_ += size;
    return S_OK;
  }
  STDMETHODIMP SetText(TfEditCookie, DWORD, const WCHAR*, LONG) override { return E_NOTIMPL; }
  STDMETHODIMP GetFormattedText(TfEditCookie, IDataObject**) override { return E_NOTIMPL; }
  STDMETHODIMP GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown**) override { return E_NOTIMPL; }
  STDMETHODIMP InsertEmbedded(TfEditCookie, DWORD, IDataObject*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStart(TfEditCookie, LONG desired, LONG* shifted, const TF_HALTCOND*) override {
    if (!shifted) return E_POINTER;
    const LONG actual =
        std::clamp(desired, -static_cast<LONG>(start_), static_cast<LONG>(end_ - start_));
    start_ = static_cast<size_t>(static_cast<LONG>(start_) + actual);
    *shifted = actual;
    return S_OK;
  }
  STDMETHODIMP ShiftEnd(TfEditCookie, LONG desired, LONG* shifted, const TF_HALTCOND*) override {
    if (!shifted) return E_POINTER;
    const LONG actual = std::clamp(desired, -static_cast<LONG>(end_ - start_),
                                   static_cast<LONG>(document_->text.size() - end_));
    end_ = static_cast<size_t>(static_cast<LONG>(end_) + actual);
    *shifted = actual;
    return S_OK;
  }
  STDMETHODIMP ShiftStartToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftEndToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStartRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftEndRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP IsEmpty(TfEditCookie, BOOL* out) override {
    if (!out) return E_POINTER;
    *out = start_ == end_ ? TRUE : FALSE;
    return S_OK;
  }
  STDMETHODIMP Collapse(TfEditCookie, TfAnchor anchor) override {
    if (anchor == TF_ANCHOR_START)
      end_ = start_;
    else
      start_ = end_;
    return S_OK;
  }
  STDMETHODIMP IsEqualStart(TfEditCookie, ITfRange*, TfAnchor, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP IsEqualEnd(TfEditCookie, ITfRange*, TfAnchor, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP CompareStart(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
  STDMETHODIMP CompareEnd(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
  STDMETHODIMP AdjustForInsert(TfEditCookie, ULONG, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP GetGravity(TfGravity*, TfGravity*) override { return E_NOTIMPL; }
  STDMETHODIMP SetGravity(TfEditCookie, TfGravity, TfGravity) override { return E_NOTIMPL; }
  STDMETHODIMP Clone(ITfRange** out) override {
    if (!out) return E_POINTER;
    *out = new TestRange(context_, document_, start_, end_);
    return S_OK;
  }
  STDMETHODIMP GetContext(ITfContext** out) override;

 private:
  ~TestRange() = default;

  TestContext* context_;
  std::shared_ptr<Document> document_;
  size_t start_;
  size_t end_;
  ULONG refs_{1};
};

class TestContext final : public ITfContext {
 public:
  explicit TestContext(std::shared_ptr<Document> document) : document_(std::move(document)) {}

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfContext) return E_NOINTERFACE;
    *out = static_cast<ITfContext*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override { return --refs_; }
  STDMETHODIMP RequestEditSession(TfClientId, ITfEditSession* edit, DWORD flags,
                                  HRESULT* result) override {
    if (!result) return E_POINTER;
    flags_seen.push_back(flags);
    *result = permit ? edit->DoEditSession(1) : TF_E_LOCKED;
    return S_OK;
  }
  STDMETHODIMP InWriteSession(TfClientId, BOOL* out) override {
    if (!out) return E_POINTER;
    *out = FALSE;
    return S_OK;
  }
  STDMETHODIMP GetSelection(TfEditCookie, ULONG, ULONG count, TF_SELECTION* out,
                            ULONG* fetched) override {
    if (!fetched) return E_POINTER;
    *fetched = 0;
    if (!selection || !count || !out) return E_NOTIMPL;
    out[0] = {};
    out[0].range = new TestRange(this, document_, *selection, *selection);
    *fetched = 1;
    return S_OK;
  }
  STDMETHODIMP SetSelection(TfEditCookie, ULONG, const TF_SELECTION*) override { return E_NOTIMPL; }
  STDMETHODIMP GetStart(TfEditCookie, ITfRange** out) override {
    if (!out) return E_POINTER;
    *out = new TestRange(this, document_, 0, 0);
    return S_OK;
  }
  STDMETHODIMP GetEnd(TfEditCookie, ITfRange**) override { return E_NOTIMPL; }
  STDMETHODIMP GetActiveView(ITfContextView**) override { return E_NOTIMPL; }
  STDMETHODIMP EnumViews(IEnumTfContextViews**) override { return E_NOTIMPL; }
  STDMETHODIMP GetStatus(TF_STATUS*) override { return E_NOTIMPL; }
  STDMETHODIMP GetProperty(REFGUID, ITfProperty**) override { return E_NOTIMPL; }
  STDMETHODIMP GetAppProperty(REFGUID, ITfReadOnlyProperty**) override { return E_NOTIMPL; }
  STDMETHODIMP TrackProperties(const GUID**, ULONG, const GUID**, ULONG,
                               ITfReadOnlyProperty**) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP EnumProperties(IEnumTfProperties**) override { return E_NOTIMPL; }
  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr**) override { return E_NOTIMPL; }
  STDMETHODIMP CreateRangeBackup(TfEditCookie, ITfRange*, ITfRangeBackup**) override {
    return E_NOTIMPL;
  }

  bool permit{true};
  // The selection start; nullopt makes GetSelection fail.
  std::optional<size_t> selection;
  std::vector<DWORD> flags_seen;

 private:
  std::shared_ptr<Document> document_;
  ULONG refs_{1};
};

STDMETHODIMP TestRange::GetContext(ITfContext** out) {
  if (!out) return E_POINTER;
  *out = context_;
  context_->AddRef();
  return S_OK;
}

std::shared_ptr<Document> MakeDocument(std::wstring text) {
  auto document = std::make_shared<Document>();
  document->text = std::move(text);
  return document;
}

TEST(ProofreadWindowTest, KeepsTextThatFitsWhole) {
  const auto window = SelectProofreadWindow(L"今日は晴れ。", 2, 1024);
  EXPECT_EQ(window.start, 0u);
  EXPECT_EQ(window.length, 6u);
}

TEST(ProofreadWindowTest, GrowsAlternatelyAroundTheAnchor) {
  const auto window = SelectProofreadWindow(L"0123456789", 5, 4);
  EXPECT_EQ(window.start, 3u);
  EXPECT_EQ(window.length, 4u);
}

TEST(ProofreadWindowTest, CountsUtf8BytesOfJapaneseText) {
  // Each kana is 3 UTF-8 bytes; 7 bytes hold two.
  const auto window = SelectProofreadWindow(L"あいう", 1, 7);
  EXPECT_EQ(window.start, 0u);
  EXPECT_EQ(window.length, 2u);
}

TEST(ProofreadWindowTest, NeverSplitsASurrogatePair) {
  const std::wstring text = L"a\U0001F600b";  // a, D83D, DE00, b
  // An anchor inside the pair starts before it; the pair is 4 bytes.
  auto window = SelectProofreadWindow(text, 2, 4);
  EXPECT_EQ(window.start, 1u);
  EXPECT_EQ(window.length, 2u);
  // A pair that does not fit stops that side instead of taking half.
  window = SelectProofreadWindow(text, 1, 3);
  EXPECT_EQ(window.start, 0u);
  EXPECT_EQ(window.length, 1u);
  window = SelectProofreadWindow(text, 3, 3);
  EXPECT_EQ(window.start, 3u);
  EXPECT_EQ(window.length, 1u);
}

TEST(ProofreadWindowTest, CountsAnUnpairedSurrogateAsReplacementCharacter) {
  const std::wstring text{L'a', static_cast<wchar_t>(0xd800), L'b'};
  const auto window = SelectProofreadWindow(text, 0, 4);
  EXPECT_EQ(window.start, 0u);
  EXPECT_EQ(window.length, 2u);
}

TEST(ProofreadWindowTest, ClampsAnAnchorPastTheEnd) {
  const auto window = SelectProofreadWindow(L"abc", 10, 2);
  EXPECT_EQ(window.start, 1u);
  EXPECT_EQ(window.length, 2u);
}

TEST(ProofreadDocumentReaderTest, ReadsTheWholeDocumentAroundTheSelection) {
  auto document = MakeDocument(L"一行目。\r\n二行目の文。");
  TestContext context(document);
  context.selection = 4;
  const auto result = ReadProofreadDocument(&context, 7);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->text, document->text);
  EXPECT_EQ(result->anchor, 4u);
  EXPECT_FALSE(result->truncated);
  ASSERT_EQ(context.flags_seen.size(), 1u);
  EXPECT_EQ(context.flags_seen[0], static_cast<DWORD>(TF_ES_SYNC | TF_ES_READ));
}

TEST(ProofreadDocumentReaderTest, ReadsInChunksTheApplicationHandsOut) {
  auto document = MakeDocument(L"段落の本文を少しずつ返すアプリ。");
  document->max_chunk = 3;
  TestContext context(document);
  context.selection = 7;
  const auto result = ReadProofreadDocument(&context, 7);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->text, document->text);
  EXPECT_EQ(result->anchor, 7u);
  EXPECT_GT(document->get_text_calls, 2);
}

TEST(ProofreadDocumentReaderTest, StartsAtTheDocumentStartWithoutASelection) {
  auto document = MakeDocument(L"選択の無い文書。");
  TestContext context(document);
  const auto result = ReadProofreadDocument(&context, 7);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->text, document->text);
  EXPECT_EQ(result->anchor, 0u);
}

TEST(ProofreadDocumentReaderTest, CutsALargeDocumentToTheLimitAroundTheSelection) {
  constexpr size_t kSize = 200000;
  auto document = MakeDocument(std::wstring(kSize, L'a'));
  TestContext context(document);
  context.selection = kSize / 2;
  const auto result = ReadProofreadDocument(&context, 7);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->text.size(), azookey::ipc::kMaxAnomalyTextBytes);
  EXPECT_EQ(result->anchor, azookey::ipc::kMaxAnomalyTextBytes / 2);
  EXPECT_TRUE(result->truncated);
  // Reads stop at the reach on each side instead of the whole document.
  EXPECT_LE(document->code_units_read, 2 * azookey::tsf::kProofreadReadCodeUnits);
}

TEST(ProofreadDocumentReaderTest, KeepsTheUtf8LimitForJapaneseText) {
  auto document = MakeDocument(std::wstring(50000, L'あ'));
  TestContext context(document);
  context.selection = 25000;
  const auto result = ReadProofreadDocument(&context, 7);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->text.size(), azookey::ipc::kMaxAnomalyTextBytes / 3);
  EXPECT_TRUE(result->truncated);
}

TEST(ProofreadDocumentReaderTest, ReportsNothingForAnEmptyDocument) {
  TestContext context(MakeDocument(L""));
  context.selection = 0;
  EXPECT_FALSE(ReadProofreadDocument(&context, 7));
}

TEST(ProofreadDocumentReaderTest, ReportsNothingWhenTheSessionIsRefused) {
  TestContext context(MakeDocument(L"本文"));
  context.selection = 0;
  context.permit = false;
  EXPECT_FALSE(ReadProofreadDocument(&context, 7));
}

TEST(ProofreadDocumentReaderTest, ReportsNothingWhenTheApplicationCannotGiveText) {
  auto document = MakeDocument(L"本文");
  document->get_text_result = E_NOTIMPL;
  TestContext context(document);
  context.selection = 1;
  EXPECT_FALSE(ReadProofreadDocument(&context, 7));
}

TEST(ProofreadDocumentReaderTest, ReportsNothingWithoutAContext) {
  EXPECT_FALSE(ReadProofreadDocument(nullptr, 7));
}

// The hotkey (OnPreservedKey with kProofreadPreservedKeyGuid) reads nothing
// from a context whose text may not leave the TIP (DEV-1532 contract).
class ProofreadHotkeyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    service_.set_foreground_app_for_test({"notepad.exe", "Notepad", true});
    service_.set_privacy_settings_for_test(R"({"privacy":{"mode":"normal"}})");
  }

  BOOL Press(ITfContext* context) {
    BOOL eaten = FALSE;
    EXPECT_EQ(service_.OnPreservedKey(context, azookey::tsf::kProofreadPreservedKeyGuid, &eaten),
              S_OK);
    return eaten;
  }

  azookey::tsf::ScopedFocusClassificationForTest focus_{azookey::tsf::InputScopeClass::Normal};
  azookey::tsf::TextService service_;
};

TEST_F(ProofreadHotkeyTest, CapturesTheDocumentInAnOrdinaryContext) {
  auto document = MakeDocument(L"校正する本文。");
  TestContext context(document);
  context.selection = 3;
  EXPECT_TRUE(Press(&context));
  const auto& capture = service_.proofread_capture_for_test();
  ASSERT_TRUE(capture);
  EXPECT_EQ(capture->status, ProofreadCapture::Status::Captured);
  EXPECT_EQ(capture->document.text, document->text);
  EXPECT_EQ(capture->document.anchor, 3u);
}

TEST_F(ProofreadHotkeyTest, ReadsNoTextFromASecureContext) {
  service_.set_privacy_settings_for_test(R"({"privacy":{"mode":"secure"}})");
  auto document = MakeDocument(L"秘密の本文。");
  TestContext context(document);
  context.selection = 0;
  EXPECT_TRUE(Press(&context));
  const auto& capture = service_.proofread_capture_for_test();
  ASSERT_TRUE(capture);
  EXPECT_EQ(capture->status, ProofreadCapture::Status::Blocked);
  EXPECT_TRUE(capture->document.text.empty());
  EXPECT_EQ(document->get_text_calls, 0);
}

TEST_F(ProofreadHotkeyTest, ReadsNoTextWhereLearningIsNotAllowed) {
  service_.set_privacy_settings_for_test(R"({"privacy":{"mode":"private"}})");
  auto document = MakeDocument(L"学習しない文脈の本文。");
  TestContext context(document);
  context.selection = 0;
  EXPECT_TRUE(Press(&context));
  const auto& capture = service_.proofread_capture_for_test();
  ASSERT_TRUE(capture);
  EXPECT_EQ(capture->status, ProofreadCapture::Status::Blocked);
  EXPECT_EQ(document->get_text_calls, 0);
}

TEST_F(ProofreadHotkeyTest, ReportsAnApplicationWithoutReadableText) {
  auto document = MakeDocument(L"");
  TestContext context(document);
  context.selection = 0;
  EXPECT_TRUE(Press(&context));
  const auto& capture = service_.proofread_capture_for_test();
  ASSERT_TRUE(capture);
  EXPECT_EQ(capture->status, ProofreadCapture::Status::Unavailable);
}

TEST_F(ProofreadHotkeyTest, LeavesTheKeyToTheApplicationDuringAReading) {
  auto document = MakeDocument(L"本文");
  TestContext context(document);
  context.selection = 0;
  service_.preedit_kana_ = "か";
  EXPECT_FALSE(Press(&context));
  EXPECT_FALSE(service_.proofread_capture_for_test());
  service_.preedit_kana_.clear();
}

TEST_F(ProofreadHotkeyTest, LeavesTheKeyToTheApplicationInAlphanumericMode) {
  auto document = MakeDocument(L"本文");
  TestContext context(document);
  context.selection = 0;
  azookey::tsf::test::KeyboardStateGuard keyboard_state;
  BOOL eaten = FALSE;
  ASSERT_EQ(service_.OnKeyDown(&context, VK_OEM_ATTN, 0, &eaten), S_OK);
  ASSERT_TRUE(eaten);
  EXPECT_FALSE(Press(&context));
  EXPECT_FALSE(service_.proofread_capture_for_test());
  EXPECT_EQ(document->get_text_calls, 0);
}

TEST_F(ProofreadHotkeyTest, LeavesTheKeyToTheApplicationWithoutAContext) {
  EXPECT_FALSE(Press(nullptr));
  EXPECT_FALSE(service_.proofread_capture_for_test());
}

}  // namespace
