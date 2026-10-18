// DEV-1653: the candidate window that Space opens is built from a live = false
// query, never from the per-key live = true results cached while typing. A
// fake Host answers the two kinds differently so the window shows which one it
// came from.
#include <Windows.h>
#include <gtest/gtest.h>
#include <msctf.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "KeyboardStateGuard.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/tsf/TextService.h"

namespace {

using azookey::ipc::BuildHandshakeResponse;
using azookey::ipc::BuildQueryCandidatesResponse;
using azookey::ipc::BuildQueryLiveConversionResponse;
using azookey::ipc::BuildQueryPredictionsResponse;
using azookey::ipc::CandidateField;
using azookey::ipc::Envelope;
using azookey::ipc::HandshakeResponse;
using azookey::ipc::MessageType;
using azookey::ipc::NamedPipeServer;
using azookey::ipc::ParseQueryCandidatesRequest;
using azookey::ipc::QueryCandidatesResponse;
using azookey::ipc::QueryLiveConversionResponse;
using azookey::ipc::QueryPredictionsResponse;
using azookey::tsf::test::KeyboardStateGuard;

// Answers every edit session as done without running it: the preedit lives in
// the TextService mirror, which is all these tests read.
class SessionContext final : public ITfContext {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfContext) {
      *out = static_cast<ITfContext*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 2; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }
  STDMETHODIMP RequestEditSession(TfClientId, ITfEditSession*, DWORD, HRESULT* result) override {
    if (!result) return E_POINTER;
    *result = S_OK;
    return S_OK;
  }
  STDMETHODIMP InWriteSession(TfClientId, BOOL* out) override {
    if (!out) return E_POINTER;
    *out = FALSE;
    return S_OK;
  }
  STDMETHODIMP GetSelection(TfEditCookie, ULONG, ULONG, TF_SELECTION*, ULONG* fetched) override {
    if (!fetched) return E_POINTER;
    *fetched = 0;
    return E_NOTIMPL;
  }
  STDMETHODIMP SetSelection(TfEditCookie, ULONG, const TF_SELECTION*) override { return S_OK; }
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
  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr** document_mgr) override {
    if (document_mgr) *document_mgr = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP CreateRangeBackup(TfEditCookie, ITfRange*, ITfRangeBackup** backup) override {
    if (backup) *backup = nullptr;
    return E_NOTIMPL;
  }
};

class HandshakeTokenGuard {
 public:
  HandshakeTokenGuard() {
    char* existing = nullptr;
    size_t length = 0;
    if (_dupenv_s(&existing, &length, "AZOOKEY_IPC_HANDSHAKE_TOKEN") == 0 &&
        (!existing || !*existing)) {
      installed_ = _putenv_s("AZOOKEY_IPC_HANDSHAKE_TOKEN", "space-window-test-token") == 0;
    }
    std::free(existing);
  }
  ~HandshakeTokenGuard() {
    if (installed_) _putenv_s("AZOOKEY_IPC_HANDSHAKE_TOKEN", "");
  }

 private:
  bool installed_{false};
};

bool WaitUntil(const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return done();
}

CandidateField Field(std::string surface, std::string reading = "にほんご") {
  return {std::move(surface), std::move(reading), 1.0, "test"};
}

// The per-key answer for the whole reading is cached: Space makes every
// earlier request stale, so an answer for "に" would not be the one reused.
bool CachedWholeReading(azookey::tsf::TextService& service) {
  const auto cached = service.cached_candidates_for_test();
  return cached.size() == 1u && cached.front().reading == "にほんご";
}

// live = true answers one candidate, as the Host's live beam does; live = false
// answers the full list after `window_delay`. Predictions always answer, so
// their candidates-ready notification can reach the UI thread first.
struct FakeHost {
  explicit FakeHost(std::chrono::milliseconds window_delay = std::chrono::milliseconds(0))
      : window_delay_(window_delay) {}

  bool Start(const std::string& pipe_name) {
    return server.Start(pipe_name, [this](const Envelope& request) -> std::optional<Envelope> {
      Envelope response = request;
      switch (request.type) {
        case MessageType::Handshake: {
          HandshakeResponse payload;
          payload.host_version = "test-host";
          payload.accepted = true;
          payload.capabilities = {"query_live_conversion", "query_predictions"};
          response.payload_json = BuildHandshakeResponse(payload);
          return response;
        }
        case MessageType::QueryCandidates: {
          const auto payload = ParseQueryCandidatesRequest(request.payload_json);
          QueryCandidatesResponse result;
          if (payload && payload->live) {
            ++live_queries;
            // Each per-key answer carries the reading it was asked for.
            result.candidates = {Field("日本語", payload->reading)};
          } else {
            ++window_queries;
            std::this_thread::sleep_for(window_delay_);
            {
              std::unique_lock lock(hold_mutex_);
              hold_cv_.wait(lock, [this] { return !hold_window_; });
            }
            result.candidates = {Field("日本語"), Field("二本後"), Field("ニホンゴ")};
          }
          response.payload_json = BuildQueryCandidatesResponse(result);
          return response;
        }
        case MessageType::QueryLiveConversion: {
          ++live_conversions;
          QueryLiveConversionResponse result;
          result.surface = "日本語";
          result.confidence = 0.9;
          response.payload_json = BuildQueryLiveConversionResponse(result);
          return response;
        }
        case MessageType::QueryPredictions: {
          ++prediction_queries;
          QueryPredictionsResponse result;
          result.predictions.push_back({"日本語入力", "にほんごにゅうりょく", 1.0, "model", ""});
          response.payload_json = BuildQueryPredictionsResponse(result);
          return response;
        }
        default:
          return std::nullopt;
      }
    });
  }

  NamedPipeServer server;
  std::atomic<int> live_queries{0};
  std::atomic<int> live_conversions{0};
  std::atomic<int> window_queries{0};
  std::atomic<int> prediction_queries{0};

  // Holds every window answer until ReleaseWindow, so a test can run UI work
  // while the answer is known to be outstanding.
  void HoldWindow() {
    std::lock_guard lock(hold_mutex_);
    hold_window_ = true;
  }
  void ReleaseWindow() {
    {
      std::lock_guard lock(hold_mutex_);
      hold_window_ = false;
    }
    hold_cv_.notify_all();
  }

 private:
  std::chrono::milliseconds window_delay_;
  std::mutex hold_mutex_;
  std::condition_variable hold_cv_;
  bool hold_window_{false};
};

std::string PipeName(const char* tag) {
  return std::string("\\\\.\\pipe\\azookey-space-window-") + tag + "-" +
         std::to_string(GetCurrentProcessId());
}

struct HarnessOptions {
  bool live_conversion{false};
  bool prediction{false};
  bool secure{false};
  // A generous deadline keeps a slow (coverage) run from timing out a query
  // the test expects to arrive; the late-answer tests shorten it instead.
  uint32_t query_deadline_ms{5000};
};

class SpaceWindowHarness {
 public:
  SpaceWindowHarness(const std::string& pipe_name, HarnessOptions options) {
    service.set_foreground_app_for_test({"notepad.exe", "Notepad", true});
    if (options.secure) service.set_privacy_settings_for_test(R"({"privacy":{"mode":"secure"}})");
    service.set_live_conversion_for_test(options.live_conversion);
    service.set_prediction_enabled_for_test(options.prediction);
    service.set_query_deadline_ms_for_test(options.query_deadline_ms);
    service.set_ipc_pipe_name_for_test(pipe_name);
    service.start_ipc_worker_for_test();
  }
  ~SpaceWindowHarness() {
    service.stop_ipc_worker_for_test();
    service.Deactivate();
  }

  BOOL Press(WPARAM key) {
    keyboard_state.Reapply();
    BOOL eaten = FALSE;
    EXPECT_TRUE(SUCCEEDED(service.OnKeyDown(&context, key, 0, &eaten))) << "key " << key;
    return eaten;
  }

  void TypeNihongo() {
    for (const WPARAM key : {'N', 'I', 'H', 'O', 'N', 'G', 'O'}) ASSERT_TRUE(Press(key));
    ASSERT_EQ(service.input_state_for_test().confirmed_kana(), "にほんご");
  }

  // Runs every candidates-ready notification on the UI thread, as the message
  // loop would, until the window shows `count` candidates.
  std::vector<std::string> WaitForWindow(size_t count) {
    std::vector<std::string> surfaces;
    WaitUntil([&] {
      service.process_candidates_ready_for_test();
      return service.shown_candidates_for_test().size() == count;
    });
    for (const auto& field : service.shown_candidates_for_test()) surfaces.push_back(field.surface);
    return surfaces;
  }

  // Waits for any window, for the late-answer tests whose fallback length
  // depends on the local katakana forms.
  std::vector<std::string> WaitForAnyWindow() {
    std::vector<std::string> surfaces;
    WaitUntil([&] {
      service.process_candidates_ready_for_test();
      return !service.shown_candidates_for_test().empty();
    });
    for (const auto& field : service.shown_candidates_for_test()) surfaces.push_back(field.surface);
    return surfaces;
  }

  HandshakeTokenGuard token;
  KeyboardStateGuard keyboard_state;
  SessionContext context;
  azookey::tsf::TextService service;
};

const std::vector<std::string> kWindow = {"日本語", "二本後", "ニホンゴ"};

bool Contains(const std::vector<std::string>& surfaces, const std::string& surface) {
  return std::find(surfaces.begin(), surfaces.end(), surface) != surfaces.end();
}

TEST(TsfTipSpaceCandidateWindowTest, OpensFromTheWindowQueryEvenWithAPerKeyCache) {
  const auto pipe_name = PipeName("cache");
  FakeHost host;
  ASSERT_TRUE(host.Start(pipe_name));
  {
    SpaceWindowHarness h(pipe_name, {});
    h.TypeNihongo();
    // The per-key live answer has arrived, so the old path would open from it.
    ASSERT_TRUE(WaitUntil([&] { return CachedWholeReading(h.service); }));
    ASSERT_GT(host.live_queries.load(), 0);

    EXPECT_TRUE(h.Press(VK_SPACE));
    EXPECT_EQ(h.WaitForWindow(kWindow.size()), kWindow);
    EXPECT_EQ(host.window_queries.load(), 1);
  }
  host.server.Stop();
}

TEST(TsfTipSpaceCandidateWindowTest, LiveConversionAlsoOpensFromTheWindowQuery) {
  const auto pipe_name = PipeName("live");
  FakeHost host;
  ASSERT_TRUE(host.Start(pipe_name));
  {
    SpaceWindowHarness h(pipe_name, {.live_conversion = true});
    h.TypeNihongo();
    EXPECT_TRUE(h.Press(VK_SPACE));
    EXPECT_EQ(h.WaitForWindow(kWindow.size()), kWindow);
    EXPECT_EQ(host.window_queries.load(), 1);
    EXPECT_EQ(host.live_queries.load(), 0);
  }
  host.server.Stop();
}

// 5f25ceb4: a prediction answer (or any other candidates-ready notification)
// handled before the window answer must not end the wait for it.
TEST(TsfTipSpaceCandidateWindowTest, EarlierReadyNotificationsDoNotEndTheWait) {
  const auto pipe_name = PipeName("ready");
  FakeHost host;
  ASSERT_TRUE(host.Start(pipe_name));
  host.HoldWindow();
  {
    SpaceWindowHarness h(pipe_name, {.prediction = true});
    h.TypeNihongo();
    ASSERT_TRUE(WaitUntil([&] { return CachedWholeReading(h.service); }));

    EXPECT_TRUE(h.Press(VK_SPACE));
    // The Host holds the window answer while these notifications run.
    ASSERT_TRUE(WaitUntil([&] { return host.window_queries.load() == 1; }));
    for (int i = 0; i < 10; ++i) {
      h.service.process_candidates_ready_for_test();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GT(host.prediction_queries.load(), 0);
    EXPECT_TRUE(h.service.input_state_for_test().awaiting_candidates());
    host.ReleaseWindow();
    EXPECT_EQ(h.WaitForWindow(kWindow.size()), kWindow);
  }
  host.ReleaseWindow();
  host.server.Stop();
}

// A window query past the TIP deadline puts the per-key live answer for the
// same reading ahead of the TIP-local kana candidates.
TEST(TsfTipSpaceCandidateWindowTest, LateWindowQueryPutsThePerKeyCandidatesFirst) {
  const auto pipe_name = PipeName("late");
  FakeHost host(std::chrono::milliseconds(1500));
  ASSERT_TRUE(host.Start(pipe_name));
  {
    SpaceWindowHarness h(pipe_name, {.query_deadline_ms = 300});
    h.TypeNihongo();
    ASSERT_TRUE(WaitUntil([&] { return CachedWholeReading(h.service); }));

    EXPECT_TRUE(h.Press(VK_SPACE));
    const auto window = h.WaitForAnyWindow();
    ASSERT_FALSE(window.empty());
    EXPECT_EQ(window.front(), "日本語");
    EXPECT_TRUE(Contains(window, "にほんご"));
    EXPECT_EQ(host.window_queries.load(), 1);
  }
  host.server.Stop();
}

// With live conversion the preedit shows the live surface; a late window
// answer keeps it first instead of reverting the preedit to kana.
TEST(TsfTipSpaceCandidateWindowTest, LateWindowQueryKeepsTheLiveConversionSurface) {
  const auto pipe_name = PipeName("late-live");
  // The live conversion answers must come back inside the deadline even on a
  // slow (coverage) run, so this one deadline is longer than the others.
  FakeHost host(std::chrono::milliseconds(4000));
  ASSERT_TRUE(host.Start(pipe_name));
  {
    SpaceWindowHarness h(pipe_name, {.live_conversion = true, .query_deadline_ms = 1000});
    h.TypeNihongo();
    // The answer for the whole reading has to be in before Space: Space makes
    // every earlier request stale, and a live answer for "に" does not count.
    ASSERT_TRUE(WaitUntil(
        [&] { return h.service.live_conversion_result_reading_for_test() == "にほんご"; }));

    EXPECT_TRUE(h.Press(VK_SPACE));
    const auto window = h.WaitForAnyWindow();
    ASSERT_FALSE(window.empty());
    EXPECT_EQ(window.front(), "日本語");
    EXPECT_TRUE(Contains(window, "にほんご"));
  }
  host.server.Stop();
}

// A secure context never reuses an answer ranked with its context or learning.
TEST(TsfTipSpaceCandidateWindowTest, SecureLateWindowQueryUsesOnlyLocalCandidates) {
  const auto pipe_name = PipeName("late-secure");
  FakeHost host(std::chrono::milliseconds(1500));
  ASSERT_TRUE(host.Start(pipe_name));
  {
    SpaceWindowHarness h(pipe_name, {.secure = true, .query_deadline_ms = 300});
    h.TypeNihongo();
    ASSERT_TRUE(WaitUntil([&] { return CachedWholeReading(h.service); }));

    EXPECT_TRUE(h.Press(VK_SPACE));
    const auto window = h.WaitForAnyWindow();
    ASSERT_FALSE(window.empty());
    EXPECT_EQ(window.front(), "にほんご");
    EXPECT_FALSE(Contains(window, "日本語"));
  }
  host.server.Stop();
}

// A new connection may be a new Host, so the per-key answer does not survive it.
TEST(TsfTipSpaceCandidateWindowTest, ReconnectDropsThePerKeyCandidates) {
  const auto pipe_name = PipeName("late-reconnect");
  FakeHost host(std::chrono::milliseconds(1500));
  ASSERT_TRUE(host.Start(pipe_name));
  {
    SpaceWindowHarness h(pipe_name, {.query_deadline_ms = 300});
    h.TypeNihongo();
    ASSERT_TRUE(WaitUntil([&] { return CachedWholeReading(h.service); }));
    h.service.stop_ipc_worker_for_test();
    h.service.start_ipc_worker_for_test();

    EXPECT_TRUE(h.Press(VK_SPACE));
    const auto window = h.WaitForAnyWindow();
    ASSERT_FALSE(window.empty());
    EXPECT_EQ(window.front(), "にほんご");
    EXPECT_FALSE(Contains(window, "日本語"));
  }
  host.server.Stop();
}

}  // namespace
