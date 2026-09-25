#include <Windows.h>
#include <ctffunc.h>
#include <gtest/gtest.h>
#include <msctf.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/tsf/ReconversionFunction.h"
#include "azookey/tsf/TextService.h"

namespace {

class TestContext final : public ITfContext {
 public:
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
    out[0].range = selection;
    selection->AddRef();
    *fetched = 1;
    return S_OK;
  }
  STDMETHODIMP SetSelection(TfEditCookie, ULONG, const TF_SELECTION*) override { return E_NOTIMPL; }
  STDMETHODIMP GetStart(TfEditCookie, ITfRange**) override { return E_NOTIMPL; }
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
  ITfRange* selection{nullptr};
  std::vector<DWORD> flags_seen;

 private:
  ULONG refs_{1};
};

class TestRange final : public ITfRange {
 public:
  TestRange(TestContext* context, std::shared_ptr<std::wstring> text, size_t start, size_t end,
            bool heap = false, size_t max_chunk = static_cast<size_t>(-1))
      : context_(context),
        text_(std::move(text)),
        start_(start),
        end_(end),
        heap_(heap),
        max_chunk_(max_chunk) {}
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
    if (!refs && heap_) delete this;
    return refs;
  }
  STDMETHODIMP GetText(TfEditCookie, DWORD flags, WCHAR* out, ULONG max, ULONG* length) override {
    if (!out || !length) return E_POINTER;
    const size_t size = std::min({static_cast<size_t>(max), end_ - start_, max_chunk_});
    std::copy_n(text_->data() + start_, size, out);
    *length = static_cast<ULONG>(size);
    if (flags & TF_TF_MOVESTART) start_ += size;
    return S_OK;
  }
  STDMETHODIMP SetText(TfEditCookie, DWORD, const WCHAR* value, LONG length) override {
    if (!value || length < 0) return E_INVALIDARG;
    text_->replace(start_, end_ - start_, value, length);
    end_ = start_ + length;
    return S_OK;
  }
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
                                   static_cast<LONG>(text_->size() - end_));
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
  STDMETHODIMP Collapse(TfEditCookie, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP IsEqualStart(TfEditCookie, ITfRange*, TfAnchor, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP IsEqualEnd(TfEditCookie, ITfRange*, TfAnchor, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP CompareStart(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
  STDMETHODIMP CompareEnd(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
  STDMETHODIMP AdjustForInsert(TfEditCookie, ULONG, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP GetGravity(TfGravity*, TfGravity*) override { return E_NOTIMPL; }
  STDMETHODIMP SetGravity(TfEditCookie, TfGravity, TfGravity) override { return E_NOTIMPL; }
  STDMETHODIMP Clone(ITfRange** out) override {
    if (!out) return E_POINTER;
    *out = new TestRange(context_, text_, start_, end_, true, max_chunk_);
    return S_OK;
  }
  STDMETHODIMP GetContext(ITfContext** out) override {
    if (!out) return E_POINTER;
    *out = context_;
    context_->AddRef();
    return S_OK;
  }

 private:
  TestContext* context_;
  std::shared_ptr<std::wstring> text_;
  size_t start_;
  size_t end_;
  bool heap_;
  size_t max_chunk_;
  ULONG refs_{1};
};

TEST(ReconversionFunctionTest, QueryRangeExpandsCaretToJapaneseWord) {
  TestContext context;
  auto text = std::make_shared<std::wstring>(L"abc明日xyz");
  TestRange range(&context, text, 5, 5);
  auto* function = new azookey::tsf::ReconversionFunction(1, {});
  ITfRange* expanded = nullptr;
  BOOL convertible = FALSE;
  ASSERT_EQ(function->QueryRange(&range, &expanded, &convertible), S_OK);
  ASSERT_EQ(convertible, TRUE);
  ASSERT_NE(expanded, nullptr);
  WCHAR buffer[16] = {};
  ULONG length = 0;
  ASSERT_EQ(expanded->GetText(1, 0, buffer, 16, &length), S_OK);
  EXPECT_EQ(std::wstring(buffer, length), L"明日");
  EXPECT_EQ(context.flags_seen[0], TF_ES_SYNC | TF_ES_READ);
  expanded->Release();
  function->Release();
}

TEST(ReconversionFunctionTest, CandidateListAndReconvertUseProviderAndEditSession) {
  TestContext context;
  auto text = std::make_shared<std::wstring>(L"明日");
  TestRange range(&context, text, 0, 2);
  std::wstring seen;
  auto* function = new azookey::tsf::ReconversionFunction(
      1, [&](ITfRange*, const std::wstring& surface, std::vector<std::wstring>& candidates) {
        seen = surface;
        candidates = {L"あした", L"アシタ"};
        return S_OK;
      });
  ITfCandidateList* list = nullptr;
  ASSERT_EQ(function->GetReconversion(&range, &list), S_OK);
  ASSERT_NE(list, nullptr);
  EXPECT_EQ(seen, L"明日");
  ULONG count = 0;
  ASSERT_EQ(list->GetCandidateNum(&count), S_OK);
  EXPECT_EQ(count, 2u);
  ITfCandidateString* candidate = nullptr;
  ASSERT_EQ(list->GetCandidate(1, &candidate), S_OK);
  BSTR value = nullptr;
  ASSERT_EQ(candidate->GetString(&value), S_OK);
  EXPECT_STREQ(value, L"アシタ");
  SysFreeString(value);
  candidate->Release();
  IEnumTfCandidates* enumeration = nullptr;
  ASSERT_EQ(list->EnumCandidates(&enumeration), S_OK);
  ITfCandidateString* first = nullptr;
  ULONG fetched = 0;
  EXPECT_EQ(enumeration->Next(1, &first, &fetched), S_OK);
  EXPECT_EQ(fetched, 1u);
  first->Release();
  enumeration->Release();
  EXPECT_EQ(list->SetResult(1, CAND_SELECTED), S_OK);
  EXPECT_EQ(*text, L"明日");
  EXPECT_EQ(list->SetResult(1, CAND_FINALIZED), S_OK);
  EXPECT_EQ(*text, L"アシタ");
  *text = L"明日";
  list->Release();
  ASSERT_EQ(function->GetReconversion(&range, &list), S_OK);
  *text = L"昨日";
  EXPECT_EQ(list->SetResult(0, CAND_FINALIZED), TF_E_NOCONVERSION);
  EXPECT_EQ(*text, L"昨日");
  *text = L"明日";
  list->Release();
  ASSERT_EQ(function->Reconvert(&range), S_OK);
  EXPECT_EQ(*text, L"あした");
  EXPECT_EQ(context.flags_seen.back(), TF_ES_SYNC | TF_ES_READWRITE);
  function->Release();
}

TEST(ReconversionFunctionTest, PartialTextReadsCannotReplaceChangedRangeByMatchingPrefix) {
  TestContext context;
  auto text = std::make_shared<std::wstring>(L"明日甲");
  TestRange range(&context, text, 0, 3, false, 1);
  std::wstring supplied_surface;
  auto* function = new azookey::tsf::ReconversionFunction(
      1, [&](ITfRange*, const std::wstring& surface, std::vector<std::wstring>& candidates) {
        supplied_surface = surface;
        candidates = {L"あした"};
        return S_OK;
      });
  ITfCandidateList* list = nullptr;
  ASSERT_EQ(function->GetReconversion(&range, &list), S_OK);
  ASSERT_NE(list, nullptr);
  EXPECT_EQ(supplied_surface, L"明日甲");
  *text = L"明日乙";
  EXPECT_EQ(list->SetResult(0, CAND_FINALIZED), TF_E_NOCONVERSION);
  EXPECT_EQ(*text, L"明日乙");
  list->Release();
  function->Release();
}

TEST(ReconversionFunctionTest, NoConversionAndDeniedEditDoNotChangeText) {
  TestContext context;
  auto text = std::make_shared<std::wstring>(L"123");
  TestRange range(&context, text, 0, 3);
  auto* function = new azookey::tsf::ReconversionFunction(1, {});
  ITfRange* expanded = reinterpret_cast<ITfRange*>(1);
  BOOL convertible = TRUE;
  EXPECT_EQ(function->QueryRange(&range, &expanded, &convertible), S_OK);
  EXPECT_EQ(expanded, nullptr);
  EXPECT_EQ(convertible, FALSE);
  context.permit = false;
  EXPECT_EQ(function->Reconvert(&range), TF_E_LOCKED);
  EXPECT_EQ(*text, L"123");
  function->Release();
}

TEST(ReconversionFunctionTest, ConversionKeyReconvertsSelectedRange) {
  TestContext context;
  auto text = std::make_shared<std::wstring>(L"明日");
  TestRange range(&context, text, 0, 2);
  context.selection = &range;
  auto* function = new azookey::tsf::ReconversionFunction(
      1, [](ITfRange*, const std::wstring& surface, std::vector<std::wstring>& candidates) {
        EXPECT_EQ(surface, L"明日");
        candidates = {L"あした"};
        return S_OK;
      });
  EXPECT_EQ(function->ReconvertSelection(&context), S_OK);
  EXPECT_EQ(*text, L"あした");
  function->Release();
}

TEST(ReconversionFunctionTest, SelectedConvertKeyQueuesHostWorkWithoutDeletingText) {
  TestContext context;
  auto text = std::make_shared<std::wstring>(L"明日");
  TestRange range(&context, text, 0, 2);
  context.selection = &range;
  azookey::tsf::TextService service;
  service.set_foreground_app_for_test({"notepad.exe", "Notepad", true});
  BOOL eaten = FALSE;
  ASSERT_EQ(service.OnTestKeyDown(&context, VK_CONVERT, 0, &eaten), S_OK);
  EXPECT_EQ(eaten, TRUE);
  eaten = FALSE;
  ASSERT_EQ(service.OnKeyDown(&context, VK_CONVERT, 0, &eaten), S_OK);
  EXPECT_EQ(eaten, TRUE);
  EXPECT_EQ(service.pending_reconversion_surface_for_test(), "明日");
  EXPECT_EQ(*text, L"明日");
  service.Deactivate();
}

TEST(ReconversionFunctionTest, FunctionProviderUsesOnlyMatchingNonsecureCache) {
  TestContext context;
  TestContext other_context;
  auto text = std::make_shared<std::wstring>(L"明日");
  TestRange range(&context, text, 0, 2);
  TestRange other_range(&other_context, text, 0, 2);
  azookey::tsf::TextService service;
  service.set_foreground_app_for_test({"notepad.exe", "Notepad", true});
  service.set_reconversion_cache_for_test(&context, L"明日", {L"あした", L"アシタ"});
  IUnknown* unknown = nullptr;
  ASSERT_EQ(service.GetFunction(GUID_NULL, IID_ITfFnReconversion, &unknown), S_OK);
  ITfFnReconversion* function = nullptr;
  ASSERT_EQ(unknown->QueryInterface(IID_ITfFnReconversion, reinterpret_cast<void**>(&function)),
            S_OK);
  unknown->Release();
  ITfCandidateList* list = nullptr;
  ASSERT_EQ(function->GetReconversion(&range, &list), S_OK);
  ASSERT_NE(list, nullptr);
  ULONG count = 0;
  ASSERT_EQ(list->GetCandidateNum(&count), S_OK);
  EXPECT_EQ(count, 2u);
  list->Release();
  list = nullptr;
  EXPECT_EQ(function->GetReconversion(&other_range, &list), TF_E_NOCONVERSION);
  EXPECT_EQ(list, nullptr);
  *text = L"昨日";
  EXPECT_EQ(function->GetReconversion(&range, &list), TF_E_NOCONVERSION);
  EXPECT_EQ(list, nullptr);
  *text = L"明日";
  service.set_foreground_app_for_test({"KeePass.exe", "KeePass", true});
  EXPECT_EQ(function->GetReconversion(&range, &list), TF_E_NOCONVERSION);
  EXPECT_EQ(list, nullptr);
  function->Release();
  service.Deactivate();
}

TEST(ReconversionFunctionTest, SelectedConvertKeyGetsHostCandidatesAndFinalizesReplacement) {
  using namespace azookey::ipc;
  char* prior_token = nullptr;
  size_t token_length = 0;
  ASSERT_EQ(_dupenv_s(&prior_token, &token_length, "AZOOKEY_IPC_HANDSHAKE_TOKEN"), 0);
  const std::string original_token = prior_token ? prior_token : "";
  std::free(prior_token);
  struct TokenRestore {
    std::string value;
    ~TokenRestore() { _putenv_s("AZOOKEY_IPC_HANDSHAKE_TOKEN", value.c_str()); }
  } restore{original_token};
  ASSERT_EQ(_putenv_s("AZOOKEY_IPC_HANDSHAKE_TOKEN", "reconversion-test-token"), 0);

  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-reconversion-wire-" + std::to_string(GetCurrentProcessId());
  std::atomic<int> reverse_count{0};
  std::atomic<int> query_count{0};
  NamedPipeServer server;
  ASSERT_TRUE(server.Start(pipe_name, [&](const Envelope& request) -> std::optional<Envelope> {
    Envelope response = request;
    if (request.type == MessageType::Handshake) {
      HandshakeResponse payload;
      payload.host_version = "test-host";
      payload.accepted = true;
      response.payload_json = BuildHandshakeResponse(payload);
      return response;
    }
    if (request.type == MessageType::ReverseConvert) {
      const auto payload = ParseReverseConvertRequest(request.payload_json);
      if (payload && payload->surface == "明日") ++reverse_count;
      response.payload_json = BuildReverseConvertResponse({"あした", 1.0});
      return response;
    }
    if (request.type == MessageType::QueryCandidates) {
      const auto payload = ParseQueryCandidatesRequest(request.payload_json);
      if (payload && payload->reading == "あした" && !payload->secure) ++query_count;
      QueryCandidatesResponse result;
      result.candidates.push_back({"明日", "あした", 1.0, "test"});
      result.candidates.push_back({"あした", "あした", 0.9, "test"});
      response.payload_json = BuildQueryCandidatesResponse(result);
      return response;
    }
    return std::nullopt;
  }));

  TestContext context;
  auto text = std::make_shared<std::wstring>(L"明日");
  TestRange range(&context, text, 0, 2);
  context.selection = &range;
  azookey::tsf::TextService service;
  service.set_foreground_app_for_test({"notepad.exe", "Notepad", true});
  service.set_ipc_pipe_name_for_test(pipe_name);
  BOOL eaten = FALSE;
  ASSERT_EQ(service.OnKeyDown(&context, VK_CONVERT, 0, &eaten), S_OK);
  ASSERT_TRUE(eaten);
  ASSERT_EQ(*text, L"明日");
  service.start_ipc_worker_for_test();
  IUnknown* unknown = nullptr;
  ASSERT_EQ(service.GetFunction(GUID_NULL, IID_ITfFnReconversion, &unknown), S_OK);
  ITfFnReconversion* function = nullptr;
  ASSERT_EQ(unknown->QueryInterface(IID_ITfFnReconversion, reinterpret_cast<void**>(&function)),
            S_OK);
  unknown->Release();
  ITfCandidateList* list = nullptr;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline &&
         function->GetReconversion(&range, &list) == TF_E_NOCONVERSION)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_NE(list, nullptr);
  EXPECT_EQ(reverse_count.load(), 1);
  EXPECT_EQ(query_count.load(), 1);
  ULONG count = 0;
  ASSERT_EQ(list->GetCandidateNum(&count), S_OK);
  EXPECT_EQ(count, 2u);
  EXPECT_EQ(*text, L"明日");
  EXPECT_EQ(list->SetResult(1, CAND_FINALIZED), S_OK);
  EXPECT_EQ(*text, L"あした");
  list->Release();
  function->Release();
  service.stop_ipc_worker_for_test();
  service.Deactivate();
  server.Stop();
}

}  // namespace
