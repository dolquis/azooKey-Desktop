#include <Windows.h>
#include <gtest/gtest.h>
#include <msctf.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "KeyboardStateGuard.h"
#include "ProofreadPipeClient.h"
#include "azookey/tsf/AiInputGuard.h"
#include "azookey/tsf/ProofreadDocumentReader.h"
#include "azookey/tsf/SettingsLauncher.h"
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

  size_t start() const { return start_; }
  size_t end() const { return end_; }

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
  STDMETHODIMP SetSelection(TfEditCookie, ULONG count, const TF_SELECTION* in) override {
    if (!count || !in || !in->range) return E_INVALIDARG;
    const auto* range = dynamic_cast<TestRange*>(in->range);
    if (!range) return E_INVALIDARG;
    selected = std::make_pair(range->start(), range->end());
    return S_OK;
  }
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
  // The range SetSelection last received.
  std::optional<std::pair<size_t, size_t>> selected;
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

std::wstring g_launch_parameters;

BOOL WINAPI CaptureLaunch(SHELLEXECUTEINFOW* info) {
  g_launch_parameters = info->lpParameters ? info->lpParameters : L"";
  return TRUE;
}

bool AcceptAnyClient(HANDLE) noexcept { return true; }

namespace proofread = azookey::ipc::proofread;

// The hotkey (OnPreservedKey with kProofreadPreservedKeyGuid) reads nothing
// from a context whose text may not leave the TIP (DEV-1532 contract), and
// opens the settings app on a session pipe.
class ProofreadHotkeyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_launch_parameters.clear();
    azookey::tsf::testing::SetSettingsLauncherForTest(L"C:\\azooKey\\azookey_settings.exe",
                                                      &CaptureLaunch);
    azookey::tsf::testing::SetProofreadClientVerifierForTest(&AcceptAnyClient);
    service_.set_foreground_app_for_test({"notepad.exe", "Notepad", true});
    service_.set_privacy_settings_for_test(R"({"privacy":{"mode":"normal"}})");
  }
  void TearDown() override {
    azookey::tsf::testing::SetProofreadClientVerifierForTest(nullptr);
    azookey::tsf::testing::ClearSettingsLauncherForTest();
  }

  // The token the settings app was launched with.
  std::string LaunchedToken() const {
    constexpr std::wstring_view kPrefix = L"--proofread-session ";
    if (g_launch_parameters.rfind(kPrefix, 0) != 0) return {};
    std::string token;
    for (const wchar_t c : g_launch_parameters.substr(kPrefix.size()))
      token.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return token;
  }

  struct ClientRun {
    std::optional<proofread::DocumentMessage> document;
    std::optional<proofread::SelectResult> result;
  };

  // Plays the settings app: reads the document and, when given, sends one
  // select request, while this thread runs the TIP's message loop.
  ClientRun RunClient(std::optional<proofread::SelectRequest> request) {
    ClientRun run;
    std::atomic<bool> finished{false};
    const auto token = LaunchedToken();
    std::thread client_thread([&] {
      azookey::tsf::test::ProofreadPipeClient client;
      if (client.Connect(token)) {
        if (const auto json = client.Read(proofread::kMaxDocumentFrameBytes))
          run.document = proofread::ParseDocumentMessage(*json);
        if (request && client.Write(proofread::BuildSelectRequest(*request))) {
          if (const auto json = client.Read(proofread::kMaxRequestFrameBytes))
            run.result = proofread::ParseSelectResponse(*json);
        }
      }
      finished = true;
    });
    EXPECT_TRUE(azookey::tsf::test::PumpUntil([&] { return finished.load(); }));
    client_thread.join();
    return run;
  }

  BOOL Press(ITfContext* context) {
    BOOL eaten = FALSE;
    EXPECT_EQ(service_.OnPreservedKey(context, azookey::tsf::kProofreadPreservedKeyGuid, &eaten),
              S_OK);
    return eaten;
  }

  // A session keeps a reference to its context, so the contexts outlive the
  // service (members are destroyed in reverse order).
  TestContext& NewContext(std::shared_ptr<Document> document) {
    return contexts_.emplace_back(std::move(document));
  }

  azookey::tsf::ScopedFocusClassificationForTest focus_{azookey::tsf::InputScopeClass::Normal};
  std::deque<TestContext> contexts_;
  azookey::tsf::TextService service_;
};

TEST_F(ProofreadHotkeyTest, CapturesTheDocumentInAnOrdinaryContext) {
  auto document = MakeDocument(L"校正する本文。");
  auto& context = NewContext(document);
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
  auto& context = NewContext(document);
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
  auto& context = NewContext(document);
  context.selection = 0;
  EXPECT_TRUE(Press(&context));
  const auto& capture = service_.proofread_capture_for_test();
  ASSERT_TRUE(capture);
  EXPECT_EQ(capture->status, ProofreadCapture::Status::Blocked);
  EXPECT_EQ(document->get_text_calls, 0);
}

TEST_F(ProofreadHotkeyTest, ReportsAnApplicationWithoutReadableText) {
  auto document = MakeDocument(L"");
  auto& context = NewContext(document);
  context.selection = 0;
  EXPECT_TRUE(Press(&context));
  const auto& capture = service_.proofread_capture_for_test();
  ASSERT_TRUE(capture);
  EXPECT_EQ(capture->status, ProofreadCapture::Status::Unavailable);
}

TEST_F(ProofreadHotkeyTest, LeavesTheKeyToTheApplicationDuringAReading) {
  auto document = MakeDocument(L"本文");
  auto& context = NewContext(document);
  context.selection = 0;
  service_.preedit_kana_ = "か";
  EXPECT_FALSE(Press(&context));
  EXPECT_FALSE(service_.proofread_capture_for_test());
  service_.preedit_kana_.clear();
}

TEST_F(ProofreadHotkeyTest, LeavesTheKeyToTheApplicationInAlphanumericMode) {
  auto document = MakeDocument(L"本文");
  auto& context = NewContext(document);
  context.selection = 0;
  azookey::tsf::test::KeyboardStateGuard keyboard_state;
  BOOL eaten = FALSE;
  ASSERT_EQ(service_.OnKeyDown(&context, VK_OEM_ATTN, 0, &eaten), S_OK);
  ASSERT_TRUE(eaten);
  EXPECT_FALSE(Press(&context));
  EXPECT_FALSE(service_.proofread_capture_for_test());
  EXPECT_EQ(document->get_text_calls, 0);
}

TEST_F(ProofreadHotkeyTest, LaunchesTheSettingsAppWithOnlyTheSessionToken) {
  auto document = MakeDocument(L"校正する本文。");
  auto& context = NewContext(document);
  context.selection = 0;
  EXPECT_TRUE(Press(&context));
  EXPECT_TRUE(proofread::IsValidSessionToken(LaunchedToken()));
  EXPECT_EQ(g_launch_parameters.find(L"本文"), std::wstring::npos);
  EXPECT_TRUE(service_.proofread_session_active_for_test());

  const auto run = RunClient(std::nullopt);
  ASSERT_TRUE(run.document);
  EXPECT_EQ(run.document->status, proofread::DocumentStatus::Captured);
  EXPECT_EQ(run.document->text, "校正する本文。");
  EXPECT_FALSE(run.document->secure);
  EXPECT_TRUE(run.document->learning_allowed);
}

TEST_F(ProofreadHotkeyTest, SecureContextOpensASessionWithoutText) {
  service_.set_privacy_settings_for_test(R"({"privacy":{"mode":"secure"}})");
  auto document = MakeDocument(L"秘密の本文。");
  auto& context = NewContext(document);
  context.selection = 0;
  EXPECT_TRUE(Press(&context));
  const auto run = RunClient(proofread::SelectRequest{0, 1});
  ASSERT_TRUE(run.document);
  EXPECT_EQ(run.document->status, proofread::DocumentStatus::Blocked);
  EXPECT_TRUE(run.document->text.empty());
  EXPECT_TRUE(run.document->secure);
  EXPECT_EQ(run.result, proofread::SelectResult::ContextGone);
  EXPECT_FALSE(context.selected);
  EXPECT_EQ(document->get_text_calls, 0);
}

TEST_F(ProofreadHotkeyTest, SelectsTheRequestedRangeOfTheCheckedText) {
  auto document = MakeDocument(L"前の行。\r\n校正する本文。");
  auto& context = NewContext(document);
  context.selection = 6;
  EXPECT_TRUE(Press(&context));
  const auto run = RunClient(proofread::SelectRequest{6, 2});
  EXPECT_EQ(run.result, proofread::SelectResult::Ok);
  ASSERT_TRUE(context.selected);
  EXPECT_EQ(context.selected->first, 6u);
  EXPECT_EQ(context.selected->second, 8u);
}

TEST_F(ProofreadHotkeyTest, RefusesToSelectAfterTheDocumentChanged) {
  auto document = MakeDocument(L"校正する本文。");
  auto& context = NewContext(document);
  context.selection = 0;
  EXPECT_TRUE(Press(&context));
  document->text[2] = L'X';
  const auto run = RunClient(proofread::SelectRequest{0, 2});
  EXPECT_EQ(run.result, proofread::SelectResult::DocumentChanged);
  EXPECT_FALSE(context.selected);
}

TEST_F(ProofreadHotkeyTest, RefusesARangeOutsideTheWindowOrInsideASurrogatePair) {
  auto document = MakeDocument(L"絵文字\U0001F600です。");
  auto& context = NewContext(document);
  context.selection = 0;
  EXPECT_TRUE(Press(&context));
  for (const auto request : {proofread::SelectRequest{6, 5}, proofread::SelectRequest{4, 1}}) {
    const auto run = RunClient(request);
    EXPECT_EQ(run.result, proofread::SelectResult::InvalidRange);
    // Each client run ends that session; start the next one.
    EXPECT_TRUE(Press(&context));
  }
  EXPECT_FALSE(context.selected);
}

TEST_F(ProofreadHotkeyTest, PressingAgainReplacesTheSession) {
  auto document = MakeDocument(L"本文");
  auto& context = NewContext(document);
  context.selection = 0;
  EXPECT_TRUE(Press(&context));
  const auto first = LaunchedToken();
  EXPECT_TRUE(Press(&context));
  const auto second = LaunchedToken();
  EXPECT_NE(first, second);
  azookey::tsf::test::ProofreadPipeClient stale;
  EXPECT_FALSE(stale.Connect(first));
  EXPECT_TRUE(service_.proofread_session_active_for_test());
}

TEST_F(ProofreadHotkeyTest, PoppingTheContextEndsItsSession) {
  auto document = MakeDocument(L"本文");
  auto& context = NewContext(document);
  context.selection = 0;
  EXPECT_TRUE(Press(&context));
  auto& other = NewContext(MakeDocument(L"別の文書"));
  ASSERT_EQ(service_.OnPopContext(&other), S_OK);
  EXPECT_TRUE(service_.proofread_session_active_for_test());
  ASSERT_EQ(service_.OnPopContext(&context), S_OK);
  EXPECT_FALSE(service_.proofread_session_active_for_test());
}

TEST_F(ProofreadHotkeyTest, NoSessionWhenTheSettingsAppCannotStart) {
  azookey::tsf::testing::SetSettingsLauncherForTest(L"C:\\azooKey\\azookey_settings.exe",
                                                    [](SHELLEXECUTEINFOW*) -> BOOL {
                                                      SetLastError(ERROR_FILE_NOT_FOUND);
                                                      return FALSE;
                                                    });
  auto document = MakeDocument(L"本文");
  auto& context = NewContext(document);
  context.selection = 0;
  EXPECT_TRUE(Press(&context));
  EXPECT_FALSE(service_.proofread_session_active_for_test());
}

TEST_F(ProofreadHotkeyTest, DisabledProcessLeavesTheKeyToTheApplication) {
  azookey::tsf::testing::SetProofreadEnabledForTest(false);
  auto document = MakeDocument(L"本文");
  auto& context = NewContext(document);
  context.selection = 0;
  EXPECT_FALSE(Press(&context));
  azookey::tsf::testing::SetProofreadEnabledForTest(std::nullopt);
  EXPECT_FALSE(service_.proofread_session_active_for_test());
  EXPECT_EQ(document->get_text_calls, 0);
}

TEST_F(ProofreadHotkeyTest, LeavesTheKeyToTheApplicationWithoutAContext) {
  EXPECT_FALSE(Press(nullptr));
  EXPECT_FALSE(service_.proofread_capture_for_test());
}

}  // namespace
