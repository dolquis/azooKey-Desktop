#pragma once

#include <Windows.h>
#include <ctffunc.h>
#include <msctf.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "azookey/core/InputState.h"
#include "azookey/core/RomajiKanaConverter.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/tsf/AiInputGuard.h"
#include "azookey/tsf/CandidateUiCoordinator.h"
#include "azookey/tsf/CharacterFormEditSession.h"
#include "azookey/tsf/DebugWindow.h"
#include "azookey/tsf/DelayedActionTimer.h"
#include "azookey/tsf/DisplayAttribute.h"
#include "azookey/tsf/ForegroundAppDetector.h"
#include "azookey/tsf/IpcConnectionState.h"
#include "azookey/tsf/LangBarInputModeItem.h"
#include "azookey/tsf/LiveCarry.h"
#include "azookey/tsf/LivePunctuation.h"
#include "azookey/tsf/OpenCloseKey.h"
#include "azookey/tsf/PredictionWindow.h"
#include "azookey/tsf/ProofreadDocumentReader.h"
#include "azookey/tsf/ProofreadSessionServer.h"
#include "azookey/tsf/ReconversionFunction.h"
#include "azookey/tsf/TipLocalSettings.h"
#include "azookey/tsf/TypoCorrectionTracker.h"
#ifdef _DEBUG
#include "azookey/tsf/DebugThreadAffinity.h"
#endif

namespace azookey::tsf {

// TSF preserved key for F10, the debug window toggle (docs/legacy-parity-spec.md §8.1).
// {3B7E9A52-6C1D-4F08-9E2A-8D5C4B1F7A63}
inline constexpr GUID kDebugWindowPreservedKeyGuid = {
    0x3b7e9a52, 0x6c1d, 0x4f08, {0x9e, 0x2a, 0x8d, 0x5c, 0x4b, 0x1f, 0x7a, 0x63}};

// TSF preserved key for Ctrl+Shift+Space, the batch correction view
// (docs/rich-features-spec.md X-3-6).
// {C8BE9EA1-D1EA-4A0F-BBCC-0277F468BCB7}
inline constexpr GUID kProofreadPreservedKeyGuid = {
    0xc8be9ea1, 0xd1ea, 0x4a0f, {0xbb, 0xcc, 0x02, 0x77, 0xf4, 0x68, 0xbc, 0xb7}};

// What the batch correction hotkey took from the focused document.
struct ProofreadCapture {
  enum class Status : uint8_t {
    // A secure or no-learning context, or one where external AI is not
    // allowed: no text was read.
    Blocked,
    // The application exposed no readable text.
    Unavailable,
    Captured,
  };
  Status status{Status::Unavailable};
  // Event privacy of the context, passed on to DetectAnomalies.
  bool secure{true};
  bool learning_allowed{false};
  ProofreadDocument document;
};

// A prediction may be accepted only if its reading extends the current kana.
std::optional<std::string> PredictionReadingSuffix(std::string_view kana,
                                                   const ipc::CandidateField& candidate);
std::vector<ipc::CandidateField> FilterAcceptablePredictions(
    std::string_view kana, std::vector<ipc::CandidateField> candidates);

// Where a prediction stopped or ended; logged once per composition each.
enum class PredictionLogReason : uint8_t {
  Disabled,
  NotAllowed,
  NoContext,
  KeyboardClosed,
  AlphanumericMode,
  CandidateWindow,
  NotComposing,
  SecureInput,
  Queued,
  HostUnsupported,
  ResponseDropped,
  StaleReading,
  StaleContext,
  StaleGeneration,
  NoAcceptablePrediction,
  WindowCreateFailed,
  WindowShowFailed,
  Shown,
};

#ifdef AZOOKEY_TSF_TESTING
namespace testing {
using TranslateOemCompositionCharacterFnForTest = std::optional<WCHAR> (*)(WPARAM, LPARAM);
using TranslateAsciiDecimalDigitFnForTest = std::optional<char> (*)(WPARAM, LPARAM);
using GetGuiThreadInfoFnForTest = BOOL(WINAPI*)(DWORD, PGUITHREADINFO);
using ClientToScreenFnForTest = BOOL(WINAPI*)(HWND, LPPOINT);
using GetPhysicalCursorPosFnForTest = BOOL(WINAPI*)(LPPOINT);
using LogicalToPhysicalPointForPerMonitorDpiFnForTest = BOOL(WINAPI*)(HWND, LPPOINT);
using GetMonitorScalePercentFnForTest = UINT (*)(POINT);

struct CaretAnchorForTest {
  POINT point;
  bool valid;
};

void FailNextComBoundaryAllocationForTest();
void ClearComBoundaryAllocationFailureForTest();
bool IsFreshQueryResultForTest(bool has_newer_request, uint64_t pending_request_id,
                               uint64_t response_request_id);
bool ConsumeComBoundaryAllocationFailureForTest();
// nullopt restores the build default (F10 claimed only by a Debug build).
void SetDebugWindowKeyEnabledForTest(std::optional<bool> enabled);
// nullopt restores the test default: the batch correction view is enabled
// whatever the integrity of the test process.
void SetProofreadEnabledForTest(std::optional<bool> enabled);
// nullopt restores the test default: the input indicator menu may write
// settings.json whatever the integrity of the test process.
void SetSettingsMenuWritableForTest(std::optional<bool> writable);
// nullptr restores the settings-app check of the session client.
void SetProofreadClientVerifierForTest(ProofreadSessionServer::ClientVerifier verifier);
// nullopt restores the 10-minute idle limit of a session.
void SetProofreadIdleTimeoutForTest(std::optional<std::chrono::milliseconds> timeout);
void FailNextPendingCommitObservationForTest();
void ClearPendingCommitObservationFailureForTest();
bool ConsumePendingCommitObservationFailureForTest();
bool IsExpectedIpcResponseForTest(const ipc::Envelope& response, uint64_t expected_request_id,
                                  ipc::MessageType expected_type);
std::optional<WCHAR> TranslateOemCompositionCharacterUsingWin32ForTest(WPARAM virtual_key,
                                                                       LPARAM key_data);
void SetTranslateOemCompositionCharacterForTest(
    TranslateOemCompositionCharacterFnForTest translate_character);
void ClearTranslateOemCompositionCharacterForTest();
void SetTranslateBracketCharacterForTest(TranslateOemCompositionCharacterFnForTest translate);
void SetTranslateCustomRomajiCharacterForTest(TranslateOemCompositionCharacterFnForTest translate);
void ClearTranslateCustomRomajiCharacterForTest();
void ClearTranslateBracketCharacterForTest();
std::optional<char> TranslateAsciiDecimalDigitUsingWin32ForTest(WPARAM virtual_key,
                                                                LPARAM key_data);
void SetTranslateAsciiDecimalDigitForTest(TranslateAsciiDecimalDigitFnForTest translate_digit);
void ClearTranslateAsciiDecimalDigitForTest();
void SetCaretWin32ApiForTest(
    GetGuiThreadInfoFnForTest get_gui_thread_info, ClientToScreenFnForTest client_to_screen,
    GetPhysicalCursorPosFnForTest get_physical_cursor_pos,
    LogicalToPhysicalPointForPerMonitorDpiFnForTest logical_to_physical_point,
    GetMonitorScalePercentFnForTest get_monitor_scale_percent);
void ClearCaretWin32ApiForTest();
CaretAnchorForTest ResolveCaretAnchorForTest(const RECT* text_ext_rect,
                                             HWND text_extent_window = nullptr);
}  // namespace testing
#endif

class EditSession;

struct TipCandidate {
  ipc::CandidateField field;
  std::string description;
  bool local{false};
};

class TextService final : public ITfTextInputProcessorEx,
                          public ITfKeyEventSink,
                          public ITfThreadMgrEventSink,
                          public ITfTextEditSink,
                          public ITfCompartmentEventSink,
                          public ITfFunctionProvider,
                          public ITfCompositionSink,
                          public ITfDisplayAttributeProvider,
                          public ITfMouseSink,
                          public ITfFnConfigure {
 public:
  TextService();
  ~TextService();

  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override;
  STDMETHODIMP Deactivate() override;
  STDMETHODIMP ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) override;

  STDMETHODIMP OnSetFocus(BOOL foreground) override;
  STDMETHODIMP OnTestKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam,
                             BOOL* eaten) override;
  STDMETHODIMP OnTestKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) override;
  STDMETHODIMP OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) override;
  STDMETHODIMP OnKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) override;
  STDMETHODIMP OnPreservedKey(ITfContext* context, REFGUID rguid, BOOL* eaten) override;

  STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pdim) override;
  STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pdim) override;
  STDMETHODIMP OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus) override;
  STDMETHODIMP OnPushContext(ITfContext* pic) override;
  STDMETHODIMP OnPopContext(ITfContext* pic) override;
  STDMETHODIMP OnEndEdit(ITfContext* context, TfEditCookie cookie, ITfEditRecord* record) override;

  STDMETHODIMP OnChange(REFGUID rguid) override;
  STDMETHODIMP GetType(GUID* type) override;
  STDMETHODIMP GetDescription(BSTR* description) override;
  STDMETHODIMP GetFunction(REFGUID group, REFIID iid, IUnknown** function) override;

  STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

  STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) override;
  STDMETHODIMP GetDisplayAttributeInfo(REFGUID guidInfo, ITfDisplayAttributeInfo** ppInfo) override;

  // A left click on a batch-conversion segment focuses it (spec §7).
  STDMETHODIMP OnMouseEvent(ULONG uEdge, ULONG uQuadrant, DWORD dwBtnStatus,
                            BOOL* pfEaten) override;

  STDMETHODIMP GetDisplayName(BSTR* name) override;
  STDMETHODIMP Show(HWND parent, LANGID langid, REFGUID profile) override;

  HRESULT RequestPreeditUpdate(ITfContext* context, bool* request_accepted = nullptr);

  // True when the active TSF thread runs in UI-less mode (Windows 11 / Office
  // route candidate UI through the application). Sourced from
  // ITfThreadMgrEx::GetActiveFlags in ActivateEx (spec §2.10) and propagated to
  // CandidateUiCoordinator for pbShow-driven candidate UI routing.
  bool ui_less_mode() const { return ui_less_mode_; }
  bool keyboard_open() const { return keyboard_open_; }
  HRESULT ToggleKeyboardOpen();
  // A user's choice of on or off (spec §4.1): never undone by the initial open
  // of a new document. `open` while already on only leaves alphanumeric mode.
  HRESULT SetKeyboardOpen(bool open, std::string_view source);

  // Accessed by EditSession.
  std::string preedit_kana_;
  ITfComposition* composition_{nullptr};
  ITfRange* composition_range_{nullptr};
  std::string composition_range_surface_;
  ITfRange* terminated_composition_range_{nullptr};
  std::string terminated_composition_surface_;
  std::string terminated_composition_previous_surface_;
  // Mouse sink over the composition while it exists (ITfMouseTracker).
  ITfMouseTracker* mouse_tracker_{nullptr};
  ITfContext* mouse_tracker_context_{nullptr};
  DWORD mouse_sink_cookie_{0};
  // Segments the last edit session painted; mouse edges refer to this layout.
  std::vector<DisplayedSegment> attributed_segments_;
  // Keeps the sink TSF is delivering to from being re-advised under it.
  bool mouse_event_in_progress_{false};
  // TfGuidAtoms for DisplayAttributeKind, registered on first use.
  TfGuidAtom display_attribute_atoms_[kDisplayAttributeKindCount]{};
  bool terminated_focus_cleanup_pending_{false};
  // DEV-1637: the document that input typed before a document switch, and
  // never shown in the old one, moved to. The next key decides whether the
  // input continues there.
  ITfDocumentMgr* carried_input_document_{nullptr};
  // The context input was last carried away from. Sessions it still has
  // queued must not write there until the TIP edits it again.
  const void* carried_from_context_{nullptr};
  bool etw_composition_end_in_progress_{false};
  std::uint64_t etw_composition_length_{0};
  bool committing_{false};
  std::string commit_surface_;
  POINT caret_pt_{0, 0};
  bool caret_pt_valid_{false};
  RECT caret_rect_{0, 0, 0, 0};
  bool caret_rect_valid_{false};
  // Context extraction is deferred to DEV-1349; the M15 wire field stays empty.
  std::string prediction_left_context_;

#ifdef AZOOKEY_TSF_TESTING
  bool candidate_window_show_pending_for_test();
  void set_bracket_settings_for_test(const core::BracketSettings& settings) {
    local_settings_.SetForTest(settings);
  }
  void set_privacy_settings_for_test(std::string_view contents) {
    local_settings_.SetPrivacyForTest(contents);
  }
  void set_live_conversion_for_test(bool enabled) {
    local_settings_.SetLiveConversionForTest(enabled);
  }
  void set_punctuation_conversion_for_test(bool enabled) {
    local_settings_.SetPunctuationConversionForTest(enabled);
  }
  void set_convert_keys_action_for_test(ConvertKeysAction action) {
    local_settings_.SetConvertKeysActionForTest(action);
  }
  void set_full_width_space_for_test(bool enabled) {
    local_settings_.SetFullWidthSpaceForTest(enabled);
  }
  bool full_width_space_waits_for_retry_for_test() const {
    return full_width_space_context_ != nullptr;
  }
  bool alnum_mode_for_test() const { return alnum_mode_; }
  void set_alnum_mode_for_test(bool alnum) { alnum_mode_ = alnum; }
  const std::string& batch_raw_romaji_for_test() const { return batch_raw_romaji_; }
  // Gives a harness that never calls ActivateEx an open/close compartment
  // (no sink); Deactivate releases it.
  void set_keyboard_open_compartment_for_test(ITfCompartment* compartment, TfClientId client_id) {
    if (compartment) compartment->AddRef();
    if (keyboard_open_compartment_) keyboard_open_compartment_->Release();
    keyboard_open_compartment_ = compartment;
    client_id_ = client_id;
  }
  LangBarInputModeItem* input_mode_langbar_item_for_test() const {
    return input_mode_langbar_.item_for_test();
  }
  // What the input indicator's click handler runs, for harnesses that never activate.
  void click_input_mode_langbar_for_test(TfLBIClick click) {
    OnInputModeLangBarClick(click, POINT{}, nullptr);
  }
  // The settings.json the input indicator menu writes; ActivateEx sets the real one.
  void set_settings_path_for_test(std::filesystem::path path) { settings_path_ = std::move(path); }
  unsigned host_config_reloads_for_test() const { return host_config_reloads_; }
  unsigned typo_tracked_keys_for_test() const { return typo_tracked_keys_; }
  void set_max_context_length_for_test(uint32_t length) {
    local_settings_.SetMaxContextLengthForTest(length);
  }
  void set_prediction_enabled_for_test(bool enabled) {
    local_settings_.SetPredictionEnabledForTest(enabled);
  }
  void set_romaji_table_for_test(std::shared_ptr<const core::CustomRomajiTable> table) {
    local_settings_.SetRomajiTableForTest(std::move(table));
  }
  HRESULT apply_prediction_reading_for_test(ITfContext* context, const std::string& reading) {
    return ApplyPredictionReading(context, reading);
  }
  uint64_t pending_prediction_generation_for_test() {
    std::lock_guard lock(ipc_mtx_);
    return ipc_prediction_request_ ? ipc_prediction_request_->generation : 0;
  }
  bool has_prediction_result_for_test() {
    std::lock_guard lock(candidates_mtx_);
    return prediction_result_.has_value();
  }
  size_t shown_prediction_count_for_test() const { return shown_predictions_.size(); }
  // One bit per PredictionLogReason logged since the composition started.
  uint32_t prediction_logged_reasons_for_test() const {
    return prediction_logged_reasons_.load(std::memory_order_relaxed);
  }
  bool prediction_query_hold_set_for_test() const { return !prediction_last_query_key_.empty(); }
  bool prediction_window_visible_for_test() const { return prediction_window_.IsVisible(); }
  void show_prediction_for_test(const ipc::CandidateField& prediction) {
    shown_predictions_.push_back(prediction);
    ShowPredictionWindow();
  }
  bool romaji_pending_for_test() const { return romaji_.HasPending(); }
  void retry_dropped_prediction_for_test() {
    const uint64_t generation = pending_prediction_generation_for_test();
    prediction_last_query_at_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    prediction_retry_generation_.store(generation, std::memory_order_release);
    OnCandidatesReady(this);
  }
  // Makes every cached prediction and the last prediction query look `age`
  // older, as if the UI thread had stalled that long since the result arrived
  // (DEV-1754).
  void age_prediction_state_for_test(std::chrono::milliseconds age) {
    for (auto& [key, entry] : prediction_cache_) entry.stored_at -= age;
    prediction_last_query_at_ -= age;
  }
  // True from the moment a prediction response is dropped until its retry has
  // been queued by a candidates-ready notification.
  bool prediction_retry_pending_for_test() const {
    return prediction_retry_generation_.load(std::memory_order_acquire) != 0 ||
           prediction_retry_scheduled_generation_ != 0;
  }
  void resolve_privacy_for_benchmark(ITfContext* context) { (void)ResolvePrivacy(context, false); }
  bool resolve_secure_for_test(ITfContext* context) {
    return ResolvePrivacy(context, false).secure;
  }
  core::AiPrivacy resolve_ai_privacy_for_test(ITfContext* context) {
    return ResolvePrivacy(context, true).ai;
  }
  bool resolve_detailed_logging_for_test(ITfContext* context) {
    return ResolvePrivacy(context, false).detailed_logging_allowed;
  }
  bool secure_toast_pending_for_test() const {
    return candidate_ui_.secure_toast_pending_for_test();
  }
  bool secure_indicator_visible_for_test() const {
    return candidate_ui_.own_window_for_test().secure_indicator_visible_for_test();
  }
  bool bracket_composition_for_test() const { return bracket_composition_; }
  const std::optional<ProofreadCapture>& proofread_capture_for_test() const {
    return proofread_capture_for_test_;
  }
  bool proofread_session_active_for_test() const {
    return proofread_session_ && proofread_session_->active();
  }
  // The state StartProofreadSession keeps for a captured document, without a
  // pipe or the settings app.
  void begin_proofread_session_for_test(ITfContext* context, ITfRange* range,
                                        std::wstring_view text) {
    EndProofreadSession();
    ++proofread_generation_;
    proofread_context_ = context;
    proofread_range_ = range;
    proofread_hash_ = ProofreadTextHash(text);
    proofread_length_ = text.size();
  }
  void end_proofread_session_for_test() { EndProofreadSession(); }
  void apply_proofread_selection_for_test(ipc::proofread::SelectRequest request) {
    ApplyProofreadSelection(request);
  }
  std::optional<ipc::proofread::SelectResult> last_proofread_select_result_for_test() const {
    return last_proofread_select_result_for_test_;
  }
  bool has_pending_proofread_reconversion_for_test() const {
    return pending_proofread_reconversion_.has_value();
  }
  void run_pending_proofread_reconversion_for_test() { RunPendingProofreadReconversion(); }
  void set_foreground_app_for_test(core::ForegroundApp app) {
    foreground_app_.SetForTest(std::move(app));
  }
  void set_cached_candidates_for_test(std::vector<ipc::CandidateField> candidates);
  void set_rewritten_cached_candidates_for_test(const std::string& reading,
                                                std::vector<ipc::CandidateField> candidates);
  std::vector<ipc::CandidateField> cached_candidates_for_test();
  std::vector<ipc::CandidateField> shown_candidates_for_test() const;
  const core::InputState& input_state_for_test() const { return input_state_; }
  std::vector<std::string> alt_readings_for_test(const std::string& reading,
                                                 size_t max_ambiguous) const {
    return AltReadingsFor(reading, max_ambiguous);
  }
  std::string observed_reading_for_test(const std::string& reading,
                                        const ipc::CandidateField& chosen) const {
    return ObservedReading(reading, chosen);
  }
  void post_window_query_for_test(ITfContext* context, const std::string& reading) {
    PostQueryCandidates(context, reading, false);
  }
  std::vector<std::string> pending_ipc_alt_readings_for_test() {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    return ipc_pending_alt_readings_;
  }
  bool candidate_window_showing_for_test() const { return candidate_ui_.IsShowing(); }
  size_t window_waiting_key_count_for_test() const { return window_waiting_keys_.size(); }
  // What the IPC worker does with an empty answer to the window query.
  void answer_window_query_empty_for_test() {
    std::lock_guard<std::mutex> lock(candidates_mtx_);
    candidates_.clear();
    candidate_window_show_pending_ = false;
  }
  std::vector<CandidateViewItem> candidate_views_for_test(
      const std::string& reading, std::vector<ipc::CandidateField> candidates) const;
  void set_number_rewriter_enabled_for_test(bool enabled) {
    number_rewriter_.store(enabled, std::memory_order_relaxed);
  }
  void set_katakana_rewriter_enabled_for_test(bool enabled) {
    katakana_rewriter_.store(enabled, std::memory_order_relaxed);
  }
  void set_selected_candidate_index_for_test(int index) { selected_candidate_idx_ = index; }
  void set_symbol_rewriter_for_test(bool enabled) { symbol_rewriter_.store(enabled); }
  void set_emoji_rewriter_for_test(bool enabled, bool trigger = true, uint32_t minimum = 1) {
    emoji_rewriter_.store(enabled);
    emoji_trigger_search_.store(trigger);
    emoji_trigger_min_query_length_.store(minimum);
  }
  std::string pending_emoji_trigger_for_test() {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    return ipc_pending_emoji_trigger_;
  }
  bool has_pending_commit_observation_for_test() const {
    return pending_commit_observation_.has_value();
  }
  bool secure_input_for_test() const { return secure_input_.load(); }
  bool prediction_allowed_for_test() const { return prediction_allowed_.load(); }
  std::optional<ipc::CommitObservationRequest> last_queued_commit_observation_for_test();
  std::optional<ipc::CommitObservationRequest> first_queued_commit_observation_for_test();
  std::optional<ipc::ObserveTypoRequest> last_queued_typo_observation_for_test();
  void set_corrected_candidates_for_test(const std::string& reading,
                                         const std::string& corrected_reading,
                                         std::vector<ipc::CandidateField> candidates,
                                         std::optional<uint64_t> request_id = std::nullopt);
  std::optional<ipc::CommitSegmentsObservationRequest> last_queued_segments_observation_for_test() {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    for (auto item = ipc_send_queue_.rbegin(); item != ipc_send_queue_.rend(); ++item)
      if (item->type == ipc::MessageType::CommitSegmentsObservation)
        return ipc::ParseCommitSegmentsObservationRequest(item->payload_json);
    return std::nullopt;
  }
  void set_commit_correction_supported_for_test(bool supported) {
    ipc_host_commit_correction_.store(supported);
  }
  bool undo_window_open_for_test() const { return undo_target_.has_value(); }
  void age_undo_window_for_test(std::chrono::steady_clock::duration age) {
    if (undo_target_) undo_target_->armed_at -= age;
  }
  std::vector<ipc::CommitCorrectionRequest> queued_corrections_for_test() {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    std::vector<ipc::CommitCorrectionRequest> requests;
    for (const auto& item : ipc_send_queue_)
      if (item.type == ipc::MessageType::CommitCorrection)
        if (auto parsed = ipc::ParseCommitCorrectionRequest(item.payload_json))
          requests.push_back(std::move(*parsed));
    return requests;
  }
  const std::string& english_raw_romaji_for_test() const { return english_raw_romaji_; }
  std::vector<ipc::ForgetLearningEntryRequest> queued_forget_requests_for_test() {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    std::vector<ipc::ForgetLearningEntryRequest> requests;
    for (const auto& item : ipc_send_queue_)
      if (item.type == ipc::MessageType::ForgetLearningEntry)
        if (auto parsed = ipc::ParseForgetLearningEntryRequest(item.payload_json))
          requests.push_back(std::move(*parsed));
    return requests;
  }
  // Posts an observation as the commit edit session would. One entry is the
  // single-candidate form; more entries are a multi-segment commit.
  void post_pending_observation_for_test(
      std::vector<std::pair<std::string, ipc::CandidateField>> segments, bool secure,
      bool learning_allowed) {
    PendingCommitObservation pending;
    pending.secure = secure;
    pending.learning_allowed = learning_allowed;
    if (segments.size() == 1) {
      pending.reading = std::move(segments[0].first);
      pending.chosen = std::move(segments[0].second);
    } else {
      for (auto& [reading, chosen] : segments)
        pending.segments.push_back(
            {std::move(reading), std::move(chosen), {}, {}, secure, learning_allowed});
    }
    pending_commit_observation_ = std::move(pending);
    PostPendingCommitObservation();
  }
  const std::vector<std::pair<std::string, std::string>>& learned_pairs_for_test() const {
    return last_learned_commit_pairs_;
  }
  size_t beep_count_for_test() const { return beep_count_; }
  bool core_input_active_for_test() const { return core_input_active_; }
  bool debug_window_visible_for_test() const { return debug_window_.IsVisible(); }
  const DebugLogBuffer& debug_log_for_test() const { return debug_window_.buffer(); }
  void set_commit_segments_supported_for_test(bool supported) {
    ipc_host_commit_segments_.store(supported);
  }
  // As Activate leaves it for TF_TMF_UIELEMENTENABLEDONLY (Edge and other
  // UI-less hosts), without a thread manager.
  void set_ui_less_mode_for_test(bool ui_less) {
    ui_less_mode_ = ui_less;
    candidate_ui_.SetUiLessMode(ui_less);
  }
  void set_english_candidates_for_test(bool enabled, bool host_supported = true) {
    local_settings_.SetInlineEnglishCandidatesForTest(enabled);
    ipc_host_english_candidates_.store(host_supported);
  }
  void set_mixed_english_for_test(bool enabled, bool host_supported = true) {
    local_settings_.SetMixedEnglishAutoConversionForTest(enabled);
    ipc_host_mixed_english_.store(host_supported);
  }
  bool pending_ipc_mixed_english_for_test() {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    return ipc_pending_mixed_english_;
  }
  bool pending_ipc_secure_for_test() {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    return ipc_pending_secure_;
  }
  bool pending_ipc_english_candidates_for_test() {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    return ipc_pending_english_candidates_;
  }
  bool pending_ipc_live_for_test() {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    return ipc_pending_live_;
  }
  std::vector<ipc::MessageType> queued_ipc_types_for_test();
  void post_commit_observation_for_test(const std::string& reading,
                                        const ipc::CandidateField& chosen) {
    PostCommitObservation(reading, chosen, {}, secure_input_.load(), true);
  }
  void show_candidate_window_from_cache_for_test();
  bool has_active_context_for_test() const { return active_context_ != nullptr; }
  std::string pending_reconversion_surface_for_test() {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    return ipc_reconversion_request_ ? ipc_reconversion_request_->surface : std::string{};
  }
  void set_reconversion_cache_for_test(ITfContext* context, std::wstring surface,
                                       std::vector<std::wstring> candidates,
                                       std::string reading = {}) {
    if (reconversion_cache_context_) reconversion_cache_context_->Release();
    reconversion_cache_context_ = context;
    if (context) context->AddRef();
    std::lock_guard<std::mutex> lock(candidates_mtx_);
    reconversion_cache_surface_ = std::move(surface);
    reconversion_cache_candidates_ = std::move(candidates);
    reconversion_cache_reading_ = std::move(reading);
  }
  void set_reconversion_ui_for_test(ITfContext* context, ITfRange* range, ITfCandidateList* list) {
    ClearReconversionState();
    if (text_edit_context_) text_edit_context_->Release();
    text_edit_context_ = context;
    if (context) context->AddRef();
    reconversion_range_ = range;
    if (range) range->AddRef();
    reconversion_list_ = list;
    if (list) list->AddRef();
  }
  void set_text_edit_context_for_test(ITfContext* context) {
    if (text_edit_context_) text_edit_context_->Release();
    text_edit_context_ = context;
    if (context) context->AddRef();
  }
  bool has_reconversion_ui_for_test() const { return reconversion_list_ != nullptr; }
  const std::vector<std::wstring>& reconversion_shown_candidates_for_test() const {
    return reconversion_shown_candidates_;
  }
  // The candidate window's click callback, without a window.
  void click_candidate_for_test(int idx) { OnCandidateClicked(idx); }
  uint64_t reconversion_generation_for_test() const { return reconversion_generation_.load(); }
  // The prefetch a selection change schedules, run now.
  void prefetch_selected_reconversion_for_test() {
    reconversion_prefetch_pending_ = true;
    PrefetchSelectedReconversion();
  }
  // The Host's answer for the open range, as the IPC worker posts it, then shown.
  void deliver_reconversion_result_for_test(std::vector<std::wstring> candidates,
                                            std::string reading = {}) {
    {
      std::lock_guard<std::mutex> lock(candidates_mtx_);
      reconversion_result_ = ReconversionResult{reconversion_generation_, reconversion_surface_,
                                                std::move(candidates), std::move(reading)};
    }
    ShowReconversionResult();
  }
  std::vector<std::wstring> reconversion_cache_candidates_for_test() {
    std::lock_guard<std::mutex> lock(candidates_mtx_);
    return reconversion_cache_candidates_;
  }
  void process_candidates_ready_for_test() { OnCandidatesReady(this); }
  bool has_reconversion_result_for_test() {
    std::lock_guard<std::mutex> lock(candidates_mtx_);
    return reconversion_result_.has_value();
  }
  void process_reconversion_result_for_test() { ShowReconversionResult(); }
  bool active_context_is_for_test(ITfContext* context) const { return active_context_ == context; }
  HRESULT commit_selected_for_test(ITfContext* context) { return CommitSelected(context); }
  HRESULT request_commit_edit_session_for_test(ITfContext* context) {
    return RequestCommitEditSession(context);
  }
  void set_batch_romaji_options_for_test(bool enabled, bool preview_romaji = false,
                                         bool auto_punctuation = false, bool ai_cleanup = false);
  core::AiPrivacy pending_ai_privacy_for_test() {
    std::lock_guard lock(ipc_mtx_);
    return ipc_pending_ai_privacy_;
  }
  bool batch_query_in_progress_for_test() const;
  void set_cached_batch_segments_for_test(std::vector<ipc::BatchConversionSegment> segments);
  std::wstring cached_batch_notice_for_test() {
    std::lock_guard<std::mutex> lock(candidates_mtx_);
    return cached_batch_notice_;
  }
  std::wstring shown_batch_notice_for_test() const { return shown_batch_notice_; }
  bool has_pending_ipc_query_for_test();
  bool pending_ipc_query_is_batch_for_test();
  uint64_t pending_ipc_request_id_for_test();
  void mark_pending_ipc_query_sent_for_test() {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    ipc_has_request_ = false;
  }
  bool pending_ipc_query_is_live_conversion_for_test() {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    return ipc_has_request_ && ipc_pending_is_live_conversion_;
  }
  void set_live_conversion_result_for_test(uint64_t request_id, std::string reading,
                                           std::string surface) {
    std::lock_guard<std::mutex> lock(candidates_mtx_);
    live_conversion_result_ =
        LiveConversionResult{request_id, std::move(reading), std::move(surface)};
  }
  bool has_live_conversion_result_for_test() {
    std::lock_guard<std::mutex> lock(candidates_mtx_);
    return live_conversion_result_.has_value();
  }
  // The reading of the delivered, not yet applied live conversion result.
  std::string live_conversion_result_reading_for_test() {
    std::lock_guard<std::mutex> lock(candidates_mtx_);
    return live_conversion_result_ ? live_conversion_result_->reading : std::string{};
  }
  void apply_live_conversion_result_for_test() { ApplyLiveConversionResult(); }
  void set_live_conversion_result_with_segments_for_test(uint64_t request_id, std::string reading,
                                                         std::string surface,
                                                         std::vector<ipc::LiveSegment> segments,
                                                         bool auto_punctuation) {
    std::lock_guard<std::mutex> lock(candidates_mtx_);
    LiveConversionResult result{request_id, std::move(reading), std::move(surface)};
    result.segments = std::move(segments);
    result.auto_punctuation = auto_punctuation;
    live_conversion_result_ = std::move(result);
  }
  void set_dynamic_punctuation_for_test(const TipDynamicPunctuationSettings& settings) {
    local_settings_.SetDynamicPunctuationForTest(settings);
  }
  bool pending_ipc_auto_punctuation_for_test() {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    return ipc_pending_auto_punctuation_;
  }
  std::string pending_ipc_punctuation_style_for_test() {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    return ipc_pending_punctuation_style_;
  }
  bool punctuation_timer_armed_for_test(DelayedActionTimer::Slot slot) const {
    return punctuation_timer_.IsArmed(slot);
  }
  uint32_t punctuation_timer_delay_for_test(DelayedActionTimer::Slot slot) const {
    return punctuation_timer_.ArmedDelayMs(slot);
  }
  // Runs the handler an expired timer would, without waiting.
  void fire_punctuation_timer_for_test(DelayedActionTimer::Slot slot) {
    punctuation_timer_.FireForTest(slot);
  }
  void set_final_request_timeout_for_test(uint32_t ms) { final_request_timeout_ms_ = ms; }
  bool final_punctuation_hold_pending_for_test() const {
    return final_punctuation_hold_.has_value();
  }
  const std::string& ipc_client_id_for_test() const { return ipc_client_id_; }
  std::string pending_ipc_reading_for_test();
  std::string pending_ipc_raw_romaji_for_test();
  void set_ipc_pipe_name_for_test(std::string pipe_name);
  void request_host_option_refresh_for_test() { RequestHostOptionRefresh(); }
  // Call before start_ipc_worker_for_test: handshakes then find no token.
  void set_handshake_token_unavailable_for_test(bool unavailable) {
    handshake_token_unavailable_for_test_.store(unavailable);
  }
  // Handshakes that found no token so far, logged or not.
  uint32_t handshake_token_miss_attempts_for_test() const {
    return handshake_token_miss_attempts_for_test_.load();
  }
  void retry_model_for_test() { RetryModelFromUi(); }
  bool batch_romaji_conversion_for_test() const {
    return batch_romaji_conversion_.load(std::memory_order_relaxed);
  }
  void post_cancel_for_test(uint64_t target_request_id) { PostCancel(target_request_id); }
  void start_ipc_worker_for_test();
  void stop_ipc_worker_for_test();
  IpcConnectionState ipc_connection_state_for_test() const {
    return ipc_connection_state_.load(std::memory_order_acquire);
  }
  bool ipc_host_unavailable_for_test() const { return IpcHostUnavailable(); }
  void set_ipc_host_unavailable_for_test(bool unavailable) {
    ipc_host_unavailable_.store(unavailable, std::memory_order_release);
  }
  // Runs on the IPC worker inside TransitionIpcConnection, after the new state
  // is published and before ipc_host_unavailable_ follows it. Set it before
  // start_ipc_worker_for_test and return from it before stopping the worker.
  using IpcStatePublishedHook = void (*)(void* context, IpcConnectionState state);
  void set_ipc_state_published_hook_for_test(IpcStatePublishedHook hook, void* context) {
    ipc_state_published_hook_for_test_ = hook;
    ipc_state_published_hook_context_for_test_ = context;
  }
  std::optional<CandidateHealthState> health_display_state_for_test() {
    std::lock_guard<std::mutex> lock(health_state_mtx_);
    return health_display_state_;
  }
  // Call before start_ipc_worker_for_test; the worker reads these unlocked.
  void set_ipc_health_timing_for_test(uint32_t interval_ms, uint32_t timeout_ms,
                                      uint32_t failure_threshold) {
    ipc_health_interval_ms_ = interval_ms;
    ipc_health_timeout_ms_ = timeout_ms;
    ipc_deadline_miss_threshold_ = failure_threshold;
  }
  // Call before start_ipc_worker_for_test: the request deadline of a
  // QueryCandidates / QueryLiveConversion, so tests do not race the 150ms.
  void set_query_deadline_ms_for_test(uint32_t deadline_ms) { query_deadline_ms_ = deadline_ms; }
  void set_window_query_deadline_ms_for_test(uint32_t deadline_ms) {
    window_query_deadline_ms_ = deadline_ms;
  }
  // Stands in for the worker marking a request in flight.
  void set_ipc_inflight_for_test(uint64_t request_id, bool window_query) {
    std::lock_guard lock(ipc_mtx_);
    ipc_inflight_id_ = request_id;
    ipc_inflight_window_query_id_ = window_query ? request_id : 0;
  }
  std::vector<uint64_t> queued_cancel_targets_for_test() {
    std::lock_guard lock(ipc_mtx_);
    std::vector<uint64_t> targets;
    for (const auto& item : ipc_send_queue_)
      if (item.type == ipc::MessageType::Cancel) targets.push_back(item.cancel_target_id);
    return targets;
  }
  POINT caret_point_for_test() const { return caret_pt_; }
  bool caret_point_valid_for_test() const { return caret_pt_valid_; }
  void set_caret_point_for_test(POINT point, bool valid) {
    caret_pt_ = point;
    caret_pt_valid_ = valid;
  }
#endif

 private:
  friend class EditSession;
  friend class BracketEditSession;
  struct EtwRegistration {
    EtwRegistration() noexcept;
    ~EtwRegistration() noexcept;
  } etw_registration_;
  TipLocalSettings local_settings_;
  ForegroundAppDetector foreground_app_;
  bool bracket_composition_{false};
  // Borrowed identity only, never dereferenced; cleared at the next key or cleanup.
  ITfContext* bracket_test_context_{nullptr};
  WPARAM bracket_test_key_{};
  HRESULT HandleBracketKey(ITfContext* context, WPARAM key, LPARAM key_data, BOOL* eaten,
                           bool test_only, bool& handled);

#ifdef _DEBUG
  DebugThreadAffinity ui_thread_affinity_;
  DebugThreadAffinity ipc_thread_affinity_;
#endif
  LONG ref_count_{1};
  ITfThreadMgr* thread_mgr_{nullptr};
  TfClientId client_id_{TF_CLIENTID_NULL};
  bool key_event_sink_advised_{false};
  DWORD thread_mgr_sink_cookie_{TF_INVALID_COOKIE};
  ITfCompartment* keyboard_open_compartment_{nullptr};
  DWORD keyboard_open_sink_cookie_{TF_INVALID_COOKIE};
  bool keyboard_open_{true};
  // COM identities of document managers that already received the initial
  // open state (spec §4.1). Compared only, never dereferenced; an entry is
  // dropped when its document manager is uninitialized.
  std::vector<const void*> keyboard_open_seen_documents_;
  const void* focused_document_identity_{nullptr};
  // A close that reaches the first-focused document within the deadline is
  // the per-context restore of a new input context, not a user action.
  const void* keyboard_open_pending_document_{nullptr};
  std::chrono::steady_clock::time_point keyboard_open_pending_deadline_{};
  // Open/alphanumeric/keyboard-disabled state last reported by
  // key_passthrough, -1 for none.
  int key_passthrough_logged_state_{-1};
  // AZOOKEY_LOG_KEYS=1 at construction: key_path records trace how each key
  // reached the composition (DEV-1637). They never carry the key or text.
  bool key_path_logging_{false};
  // Document and reasons (bits) already reported for keys that are not
  // traced one by one: typed while the IME is off, or into a secure field.
  std::optional<const void*> key_path_quiet_document_;
  unsigned key_path_quiet_reasons_{0};
  // Probe summary last reported by input_gate, so the record appears once per
  // change of field rather than once per commit or prediction.
  std::string input_gate_logged_signature_;
  // Prediction outcomes already logged since activation, one bit per reason, so
  // the log says where predictions stop without the count or timing of keys.
  std::atomic<uint32_t> prediction_logged_reasons_{0};
  bool function_provider_advised_{false};
  bool ui_less_mode_{false};
  bool alnum_mode_{false};
  // Input indicator button (spec §4.5), present between activation and deactivation.
  InputModeLangBar input_mode_langbar_;
  // Counts deactivations, so a menu that closes after one does nothing.
  uint64_t activation_epoch_{0};
  // settings.json, which the input indicator menu writes; empty when unknown.
  std::filesystem::path settings_path_;
#ifdef AZOOKEY_TSF_TESTING
  unsigned host_config_reloads_{0};
  // Keys tracked for typo learning, so a test can see a replay is not tracked twice.
  unsigned typo_tracked_keys_{0};
#endif
  // Set while an off key commits the shown text through the Enter path.
  bool commit_before_close_{false};
  // Set while OnKeyDown handles the same key again after committing the
  // shown selection (DEV-1735).
  bool retyping_after_commit_{false};
  // Keys consumed while a refused commit waits for its retry, with the key
  // state of their press, replayed once a key retries it (DEV-1750).
  struct RetryWaitingKey {
    const void* context;
    WPARAM key;
    LPARAM key_data;
    std::array<BYTE, 256> key_state;
    // False for a key held before typo and undo tracking (the held Enter of
    // dynamic-punctuation-spec §7.3), which its replay then tracks.
    bool tracked{true};
  };
  static constexpr size_t kMaxRetryWaitingKeys = 16;
  std::vector<RetryWaitingKey> retry_waiting_keys_;
  // Keys typed while the candidate window waits for its answer after Space,
  // replayed into it once the wait ends (DEV-1813). They count toward the same
  // 16 as retry_waiting_keys_.
  std::vector<RetryWaitingKey> window_waiting_keys_;
  // One log record per wait for a key it could not keep.
  bool retry_waiting_drop_logged_{false};
  // The context of a full-width space whose edit session lock was not
  // granted. The next key inserts it again once, before it is handled; focus
  // and lifecycle loss drop it (docs/tsf-deep-integration-spec.md §3.7).
  Microsoft::WRL::ComPtr<ITfContext> full_width_space_context_;
  std::optional<CharacterFormCycleState> character_form_cycle_state_;
  std::optional<CharacterFormRecentCommit> recent_character_form_commit_;

  core::RomajiKanaConverter romaji_;
  // Logical state for the ordinary (non-batch) input pipeline. The legacy
  // fields above and below remain presentation/metadata mirrors for TSF and
  // the batch/emoji/rewriter paths until those paths enter InputState.
  core::InputState input_state_;
  TypoCorrectionTracker typo_tracker_;          // UI apartment only.
  ITfContext* typo_test_key_context_{nullptr};  // Borrowed until OnKeyDown.
  WPARAM typo_test_key_{0};
  struct PendingTypoCommit {
    TypoCorrectionTracker tracker;
    std::string reading;
    uint64_t generation;
  };
  std::optional<PendingTypoCommit> pending_typo_commit_;
  std::atomic<bool> typo_learning_allowed_{false};
  std::atomic<uint64_t> typo_observation_generation_{0};
  struct CorrectedReadingResult {
    uint64_t request_id;
    std::string reading;
    std::string corrected_reading;
  };
  std::optional<CorrectedReadingResult> corrected_reading_result_;  // candidates_mtx_
  // UI thread's current key action; copied into async IPC work before return.
  std::string active_key_trace_id_;
  std::chrono::steady_clock::time_point active_key_trace_start_{};
  bool active_key_trace_active_{false};
  bool core_input_active_{true};
  bool core_action_in_progress_{false};
  std::optional<size_t> core_commit_selected_index_;
  std::string core_marked_surface_;
  std::vector<TipCandidate> core_cached_metadata_;
  std::string batch_raw_romaji_;
  // M60 raw_romaji of the ordinary (non-batch) composition: the keys as typed,
  // Shift case kept for the Host's s_case signal. Kept apart from
  // batch_raw_romaji_, which is lowercase. UI thread only.
  std::string english_raw_romaji_;
  std::shared_ptr<const core::CustomRomajiTable> batch_romaji_table_;
  bool batch_query_in_progress_{false};
  // AI axis of the batch path: an ai-cleanup conversion whose privacy was
  // reduced must not be learned from. Reset with the rest of the batch state.
  bool batch_learning_allowed_{true};
  // Route-common M46 secure state, written only by ResolvePrivacy and holding
  // the answer from the most recent commit or batch conversion. Batch
  // bookkeeping does not touch it, so ClearBatchState cannot reopen the gate in
  // PostIpcSend. Atomic because it guards a queue the IPC worker also drains.
  std::atomic<bool> secure_input_{false};
  // The privacy permission for QueryPredictions, published by ResolvePrivacy
  // for the IPC worker. predictionEnabled controls the UI separately.
  std::atomic<bool> prediction_allowed_{false};
  // detailed_logging_allowed of the latest ResolvePrivacy, captured into
  // prediction requests for the debug window's M41 gate.
  std::atomic<bool> detailed_logging_allowed_{false};
  std::atomic<bool> batch_romaji_conversion_{false};
  std::atomic<bool> batch_romaji_preview_romaji_{false};
  std::atomic<bool> batch_conversion_ai_cleanup_{false};
  std::atomic<bool> batch_auto_punctuation_{false};
  std::atomic<bool> number_rewriter_{false};
  std::atomic<bool> katakana_rewriter_{false};
  std::atomic<bool> symbol_rewriter_{false};
  std::atomic<bool> emoji_rewriter_{false};
  std::atomic<bool> emoji_trigger_search_{true};
  std::atomic<uint32_t> emoji_max_candidates_{12};
  std::atomic<uint32_t> emoji_trigger_min_query_length_{1};
  enum class EmojiMode { Inactive, Collecting, Listing };
  EmojiMode emoji_mode_{EmojiMode::Inactive};  // UI apartment only.
  std::atomic<uint32_t> max_candidates_{9};

  // Last context used for preedit updates; allows Deactivate to end composition.
  ITfContext* active_context_{nullptr};
  // Context that owns a queued commit after a sync EditSession rejection.
  ITfContext* commit_context_{nullptr};

  // Candidate UI coordinator (M5).
  CandidateUiCoordinator candidate_ui_;
  PredictionWindow prediction_window_;
  std::vector<ipc::CandidateField> shown_predictions_;  // UI thread only.
  // The cache key of the reading shown_predictions_ answers; empty when nothing is shown.
  std::string shown_prediction_key_;  // UI thread only.
  struct PredictionCacheEntry {
    std::vector<ipc::CandidateField> predictions;
    std::chrono::steady_clock::time_point stored_at;
  };
  std::unordered_map<std::string, PredictionCacheEntry> prediction_cache_;  // UI thread only.
  std::string prediction_last_query_key_;                                   // UI thread only.
  std::chrono::steady_clock::time_point prediction_last_query_at_{};
  uint64_t prediction_retry_scheduled_generation_{0};  // UI thread only.
  std::chrono::steady_clock::time_point prediction_retry_due_at_{};
  int selected_candidate_idx_{0};
  // Snapshot of candidates taken when the window was opened (used for commit
  // so that a late QueryCandidates response cannot change what is confirmed).
  std::vector<TipCandidate> shown_candidates_;
  std::vector<ipc::BatchConversionSegment> shown_batch_segments_;
  std::wstring shown_batch_notice_;  // UI thread only; fixed text, never Host body.
  std::vector<size_t> batch_segment_selections_;
  size_t batch_segment_cursor_{0};
  struct PendingCommitObservation {
    std::string reading;
    ipc::CandidateField chosen;
    std::vector<ipc::CandidateField> shown;
    std::vector<PendingCommitObservation> segments;
    bool secure{true};
    bool learning_allowed{false};
    // False for a commit the TIP forces on focus or lifecycle loss: the user
    // did not just make it, so a later Backspace is no undo of it.
    bool arms_undo{true};
    // M59 section 7.4: a segment the Host inserted without a reading. It is sent
    // as is_auto_punctuation and never observed or recorded for undo.
    bool is_auto_punctuation{false};
  };
  std::optional<PendingCommitObservation> pending_commit_observation_;
  // (reading, surface) pairs of the last learning observation actually sent,
  // the target of Ctrl+Shift+Backspace (docs/legacy-parity-spec.md §7.2).
  // UI thread only.
  std::vector<std::pair<std::string, std::string>> last_learned_commit_pairs_;
  // The (reading, surface) a Backspace right after the commit takes back: the
  // last segment of the last learning observation, when it was learned. Only
  // the first key after that commit may use it (user-learning-enhancement-spec
  // §4.1, the same one-key window as typo learning). `context` is the COM
  // identity of the committed context, compared only. The window also closes
  // on focus or lifecycle loss (ResetTypoTracking), on a selection change that
  // is not the commit's own edit (OnEndEdit), and after kUndoWindow. Every
  // place that clears last_learned_commit_pairs_ also closes it: TrackUndoKey
  // sends nothing while the pairs are empty. UI thread only.
  struct UndoTarget {
    std::string reading;
    std::string surface;
    const void* context{nullptr};
    std::chrono::steady_clock::time_point armed_at;
    // The commit's own edit has not reached OnEndEdit yet; that one
    // selection change is ours and keeps the window open.
    bool awaiting_commit_edit{true};
  };
  std::optional<UndoTarget> undo_target_;

  // M18-3 debug window (docs/legacy-parity-spec.md §8). The window is created
  // lazily on the UI thread; the IPC worker only records into its buffer.
  DebugWindow debug_window_;
  // (secure, detailed_logging_allowed) last handed to debug_window_.
  std::optional<std::pair<bool, bool>> debug_paint_policy_;
  bool debug_preserved_key_registered_{false};
  bool proofread_preserved_key_registered_{false};
  // The batch correction session (X-3-6). The TIP keeps the window range and
  // its hash, not the text, to place the selection the settings app asks for.
  std::unique_ptr<ProofreadSessionServer> proofread_session_;
  Microsoft::WRL::ComPtr<ITfContext> proofread_context_;
  Microsoft::WRL::ComPtr<ITfRange> proofread_range_;
  uint64_t proofread_hash_{0};
  size_t proofread_length_{0};
  uint64_t proofread_generation_{0};
  // Candidates for the range a select placed. The reconversion path takes its
  // own edit sessions, so it runs on the UI thread after the select session.
  struct PendingProofreadReconversion {
    uint64_t generation{0};
    Microsoft::WRL::ComPtr<ITfContext> context;
    Microsoft::WRL::ComPtr<ITfRange> range;
    std::wstring surface;
    std::vector<std::wstring> suggestions;
  };
  std::optional<PendingProofreadReconversion> pending_proofread_reconversion_;
#ifdef AZOOKEY_TSF_TESTING
  std::optional<ProofreadCapture> proofread_capture_for_test_;
  std::optional<ipc::proofread::SelectResult> last_proofread_select_result_for_test_;
#endif
#ifdef AZOOKEY_TSF_TESTING
  size_t beep_count_{0};
#endif

  // IPC worker thread state.
  std::string ipc_client_id_;
  ipc::NamedPipeClient ipc_client_;
  std::mutex ipc_mtx_;
  std::condition_variable ipc_cv_;
  std::thread ipc_thread_;
  std::atomic<bool> ipc_stop_{false};
  // Primary-connection state (spec §8.2). Written only by the IPC worker
  // through TransitionIpcConnection; atomic so the TIP thread can read it.
  std::atomic<IpcConnectionState> ipc_connection_state_{IpcConnectionState::Disconnected};
  // Failed connect/handshake attempts since the last Ready. IPC worker only.
  uint32_t ipc_failed_connect_attempts_{0};
  // True while the Host has failed (connect/handshake failure, lost connection
  // or Degraded) and not yet come back to Ready. Written by the IPC worker,
  // read by the TIP thread to switch to TIP-local candidates (spec §8.3).
  std::atomic<bool> ipc_host_unavailable_{false};
  bool IpcHostUnavailable() const { return ipc_host_unavailable_.load(std::memory_order_acquire); }
  // Silent-Host detection (spec §8.3). The Host is probed with Health only after
  // the worker has had nothing to send for ipc_health_interval_ms_. A missed
  // deadline (Health within ipc_health_timeout_ms_, QueryCandidates, batch)
  // counts toward ipc_deadline_miss_threshold_ consecutive misses, which move
  // Ready to Degraded; any timely reply resets the count and restores Ready.
  // Fixed after the worker starts; IPC worker reads them.
  uint32_t ipc_health_interval_ms_{5000};
  uint32_t ipc_health_timeout_ms_{500};
  uint32_t ipc_deadline_miss_threshold_{2};
  uint32_t ipc_consecutive_deadline_misses_{0};  // IPC worker only.
  // kQueryCandidatesFastTimeoutMs (dev-infrastructure-spec section 8.5.2);
  // fixed after the worker starts.
  uint32_t query_deadline_ms_{150};
  // The window query (live = false, Space or the emoji list) waits like
  // reconversion, so a slow Host still fills the window (DEV-1665); its miss
  // counts like any other. Fixed after the worker starts.
  uint32_t window_query_deadline_ms_{750};
  // ID of the in-flight request when it is a window query (0 = none); a newer
  // request cancels it. Protected by ipc_mtx_.
  uint64_t ipc_inflight_window_query_id_{0};
  // The last per-key answer (live = true candidates, or the live conversion
  // surface), for a window query that misses its deadline (DEV-1653). Reused
  // only for the same reading, left context and learning permission it was
  // ranked with. IPC worker only.
  struct LivePrefetch {
    std::string reading;
    std::string left_context;
    bool learning_allowed{false};
    std::vector<ipc::CandidateField> candidates;
  };
  LivePrefetch live_prefetch_;
  // Published by the IPC worker, consumed by OnCandidatesReady on the UI thread.
  std::mutex health_state_mtx_;
  std::optional<CandidateHealthState> health_display_state_;
  std::string health_display_generation_id_;
  bool health_diagnostics_pending_{false};                               // IPC worker only.
  std::chrono::steady_clock::time_point health_diagnostics_retry_at_{};  // IPC worker only.
  bool health_probe_before_diagnostics_retry_{false};                    // IPC worker only.
  std::optional<std::pair<std::string, bool>> last_health_response_;     // IPC worker only.
  // A model retry can take 30 seconds; it uses its own handshaken control pipe
  // so primary key requests never wait for UpdateConfig.
  ipc::NamedPipeClient retry_control_client_;
  std::thread retry_thread_;
  std::atomic<bool> retry_stop_{false};
  std::atomic<bool> retry_in_flight_{false};
  std::atomic<bool> retry_finished_{false};
  // A menu switch wrote liveConversion while a retry was in flight; UI thread only.
  bool host_config_reload_pending_{false};
#ifdef AZOOKEY_TSF_TESTING
  std::string ipc_pipe_name_for_test_;
  IpcStatePublishedHook ipc_state_published_hook_for_test_{nullptr};
  void* ipc_state_published_hook_context_for_test_{nullptr};
#endif
  std::string ipc_pending_reading_;
  std::string ipc_pending_left_context_;
  std::string ipc_pending_raw_romaji_;
  bool ipc_pending_english_candidates_{false};  // ipc_mtx_
  bool ipc_pending_mixed_english_{false};       // ipc_mtx_
  // ipc_mtx_; DEV-1734: other readings of the pending reading's romaji.
  std::vector<std::string> ipc_pending_alt_readings_;
  std::string ipc_pending_batch_mode_;
  std::wstring ipc_pending_batch_notice_;  // ipc_mtx_; carried into neural fallback.
  // ipc_mtx_; custom romaji table of the pending batch, set with its raw romaji.
  std::shared_ptr<const core::CustomRomajiTable> ipc_pending_batch_table_;
  std::string ipc_pending_trace_id_;       // protected by ipc_mtx_
  std::chrono::steady_clock::time_point ipc_pending_trace_start_{};  // ipc_mtx_
  uint64_t ipc_pending_id_{0};
  bool ipc_has_request_{false};
  bool ipc_pending_is_batch_{false};
  bool ipc_pending_is_live_conversion_{false};
  bool ipc_pending_apply_live_preview_{false};
  bool ipc_pending_auto_punctuation_{false};  // M59 section 7.1.
  std::string ipc_pending_punctuation_style_{"ja"};
  bool ipc_pending_live_{true};
  bool ipc_pending_secure_{true};
  bool ipc_pending_learning_allowed_{false};
  // M41 body-log permission of the pending request, for the debug window.
  bool ipc_pending_detailed_logging_allowed_{false};
  std::string ipc_pending_emoji_trigger_;
  // ID of the QueryCandidates currently sent but not yet received (0 = none).
  // Protected by ipc_mtx_; written by the worker thread, read by TIP thread.
  uint64_t ipc_inflight_id_{0};
  std::string ipc_inflight_trace_id_;  // protected by ipc_mtx_
  std::string ipc_host_generation_id_;
  bool ipc_has_known_host_generation_{false};
  bool ipc_host_oob_cancel_{false};         // IPC worker only.
  bool ipc_host_live_conversion_{false};    // IPC worker only.
  bool ipc_host_query_predictions_{false};  // IPC worker only.
  // Set by the local settings watcher when settings.json changed, so a healthy
  // connection re-runs the handshake instead of serving the batch mode and
  // punctuation policy it captured at activation (DEV-1143).
  std::atomic<bool> ipc_refresh_options_{false};
  std::atomic<bool> prediction_settings_changed_{false};
  std::atomic<bool> prediction_settings_refreshing_{false};
  std::atomic<uint64_t> prediction_retry_generation_{0};
  std::atomic<bool> ipc_host_commit_segments_{false};
  // Handshake capability "english_candidates" (M60). Without it the TIP sends
  // neither raw_romaji nor english_candidates.
  std::atomic<bool> ipc_host_english_candidates_{false};
  // Handshake capability "mixed_english" (M63). Without it the TIP sends no
  // mixed_english and no live-conversion raw_romaji.
  std::atomic<bool> ipc_host_mixed_english_{false};
  // Handshake capability "commit_correction" (DEV-1529); CommitCorrection is
  // sent only to a Host that advertises it.
  std::atomic<bool> ipc_host_commit_correction_{false};
  // A handshake that finds no token is retried on every reconnect backoff, so
  // only the first miss and one summary per interval are logged (DEV-1658).
  // Touched on the IPC thread only.
  uint32_t handshake_token_missing_count_{0};
  uint32_t handshake_token_missing_logged_count_{0};
  std::chrono::steady_clock::time_point handshake_token_missing_logged_at_{};
#ifdef AZOOKEY_TSF_TESTING
  std::atomic<bool> handshake_token_unavailable_for_test_{false};
  std::atomic<uint32_t> handshake_token_miss_attempts_for_test_{0};
#endif
  core::AiPrivacy ipc_pending_ai_privacy_;  // protected by ipc_mtx_
  std::string ipc_pending_ai_backend_;      // protected by ipc_mtx_

  // Out-of-band IPC send queue drained ahead of the pending query: Cancel is
  // fire-and-forget, CommitObservation awaits an ACK (M6, M10).
  // For Cancel items, cancel_target_id carries the target_request_id so the
  // worker can detect a cancel that targets the currently in-flight query and
  // abandon its receive (the host returns no response for canceled requests).
  // attempts counts the transport attempts an ACK-awaiting item has already
  // survived, so a resend loop that never gets an ACK gives up instead of
  // starving QueryCandidates (DEV-554).
  struct IpcSendItem {
    ipc::MessageType type;
    std::string payload_json;
    bool expects_response{false};
    uint64_t cancel_target_id{0};
    uint32_t attempts{0};
    std::string trace_id;
    uint64_t typo_generation{0};
  };
  std::vector<IpcSendItem> ipc_send_queue_;  // protected by ipc_mtx_
  struct ReconversionRequest {
    uint64_t generation;
    std::string surface;
    std::string trace_id;
  };
  std::optional<ReconversionRequest> ipc_reconversion_request_;  // ipc_mtx_
  struct PredictionRequest {
    uint64_t generation;
    std::string kana;
    std::string left_side_context;
    std::string trace_id;
    // Privacy at request time, for the debug window's M41 gate.
    bool detailed_logging_allowed{false};
    bool secure{true};
  };
  std::optional<PredictionRequest> ipc_prediction_request_;  // ipc_mtx_
  uint64_t prediction_generation_{0};                        // ipc_mtx_
  // Monotonic counter behind the CommitObservation idempotency key. Written on
  // the TIP thread, never read by the IPC worker, so an atomic suffices.
  std::atomic<uint64_t> commit_observation_seq_{0};

  // Latest Host candidates plus optional TIP-local rewrites (written by IPC
  // thread, read by TIP thread).
  std::mutex candidates_mtx_;
  std::vector<TipCandidate> candidates_;
  std::string candidates_trace_id_;                                 // candidates_mtx_
  std::chrono::steady_clock::time_point candidates_trace_start_{};  // candidates_mtx_
  bool candidates_trace_pending_{false};                            // candidates_mtx_
  std::vector<ipc::BatchConversionSegment> cached_batch_segments_;
  std::wstring cached_batch_notice_;           // candidates_mtx_.
  bool candidate_window_show_pending_{false};  // protected by candidates_mtx_
  struct LiveConversionResult {
    uint64_t request_id;
    std::string reading;
    std::string surface;
    std::string trace_id;
    std::chrono::steady_clock::time_point trace_start{};
    // M59: the response segments, and whether the request asked for punctuation.
    std::vector<ipc::LiveSegment> segments;
    bool auto_punctuation{false};
  };
  std::optional<LiveConversionResult> live_conversion_result_;  // candidates_mtx_
  struct PredictionResult {
    uint64_t generation;
    std::string kana;
    std::string left_side_context;
    std::vector<ipc::CandidateField> predictions;
  };
  std::optional<PredictionResult> prediction_result_;  // candidates_mtx_
  std::string live_display_reading_;                   // UI thread only.
  std::string live_display_surface_;                   // UI thread only.
  // M59: the segments of live_display_surface_ and whether its request asked for
  // punctuation (an idle, final or eager request). UI thread only.
  std::vector<ipc::LiveSegment> live_display_segments_;
  bool live_display_auto_punctuation_{false};
  // DEV-1806: the live answers this input showed for readings with no pending
  // romaji, oldest first and one per reading, split as LiveCarry.h describes.
  // They outlive a change of the reading, so the preedit keeps a converted
  // start while the answer for the changed reading is on its way. Cleared with
  // live_display_* everywhere except on that change. UI thread only.
  struct LiveCarry {
    std::string reading;
    std::vector<LivePiece> pieces;
  };
  static constexpr size_t kMaxLiveCarries = 8;
  std::vector<LiveCarry> live_carries_;
  // Reading of the last live request, for the idle timer. UI thread only.
  std::string live_query_reading_;
  // Idle timer (section 4.3.1) and the bounded wait of the final request
  // (section 7.3). UI thread only.
  DelayedActionTimer punctuation_timer_;
  uint32_t final_request_timeout_ms_{kFinalPunctuationRequestTimeoutMs};
  // Enter that waits for the final request; input_state_ is not advanced until
  // it resolves. UI thread only.
  // A punctuation mark that waits for the first candidate of `reading` (spec §3.5.1)
  // shares the wait: it keeps the key in `replay` and decides when it resolves.
  struct FinalPunctuationHold {
    wil::com_ptr_nothrow<ITfContext> context;
    core::HandleResult result;
    uint64_t request_id{0};
    std::optional<core::UserActionEvent> replay;
    std::string reading;
  };
  std::optional<FinalPunctuationHold> final_punctuation_hold_;
  // The live segments a punctuation mark commits, for its observation. UI thread only.
  std::optional<std::vector<ipc::LiveSegment>> punctuation_live_segments_;
  struct ReconversionResult {
    uint64_t generation;
    std::wstring surface;
    std::vector<std::wstring> candidates;
    // The reverse-converted reading, for the CommitCorrection a different
    // choice sends (user-learning-enhancement-spec §4.1). May be empty.
    std::string reading;
  };
  std::optional<ReconversionResult> reconversion_result_;    // candidates_mtx_
  std::wstring reconversion_cache_surface_;                  // candidates_mtx_
  std::vector<std::wstring> reconversion_cache_candidates_;  // candidates_mtx_
  std::string reconversion_cache_reading_;                   // candidates_mtx_
  ITfContext* reconversion_cache_context_{nullptr};          // UI thread
  ITfContext* text_edit_context_{nullptr};                   // UI thread, AddRef'd
  DWORD text_edit_cookie_{TF_INVALID_COOKIE};
  bool reconversion_prefetch_pending_{false};   // UI thread
  std::wstring reconversion_prefetch_surface_;  // UI thread, in-flight surface
  std::atomic<uint64_t> reconversion_generation_{0};
  ITfRange* reconversion_range_{nullptr};         // UI thread only
  ITfCandidateList* reconversion_list_{nullptr};  // UI thread only
  std::wstring reconversion_surface_;             // UI thread only
  // A batch correction finding's suggestions for the open range (X-3-6). They
  // are model text: shown as candidates only, never cached, never learned.
  std::vector<std::wstring> reconversion_suggestions_;  // UI thread only
  // Where each shown candidate came from; a Suggestion is replaced without a
  // CommitCorrection (user-learning-enhancement-spec §4.1).
  enum class ReconversionOrigin : uint8_t { Host, Suggestion };
  // What the open reconversion list offers, kept with reconversion_list_.
  // The vectors share the list's indices.
  std::string reconversion_shown_reading_;                      // UI thread only
  std::vector<std::wstring> reconversion_shown_candidates_;     // UI thread only
  std::vector<ReconversionOrigin> reconversion_shown_origins_;  // UI thread only

  void StartIpcWorker();
  void StopIpcWorker();
  HRESULT AdviseTextServiceSinks();
  HRESULT UnadviseTextServiceSinks();
  std::string IpcPipeName() const;
  void IpcWorkerThread();
  void IpcWorkerThreadImpl();
  std::atomic<uint32_t> ipc_worker_exception_code_{0};
  void ServeConnection();
  bool ConvertReconversion(const ReconversionRequest& request, uint64_t& next_id);
  void QueryPendingPrediction(uint64_t& next_id);
  void ConvertBatch(uint64_t generation, const std::string& reading, const std::string& raw_romaji,
                    const std::string& mode, const std::string& trace_id, bool english_candidates,
                    bool secure, std::shared_ptr<const core::CustomRomajiTable> romaji_table);
  bool PerformHandshake();
  bool PerformHandshake(ipc::NamedPipeClient& client, uint32_t timeout_ms,
                        const std::string& trace_id, bool update_host_options,
                        uint64_t request_id = 1);
  bool SendCancelOutOfBand(uint64_t target_request_id, const std::string& trace_id);
  bool SendCancelOutOfBand(uint64_t target_request_id, uint32_t connect_timeout_ms,
                           uint32_t handshake_timeout_ms, const std::string& trace_id);
  bool WaitForReconnectOrStop(uint32_t delay_ms);
  void TransitionIpcConnection(IpcConnectionEvent event);
  enum class HealthProbeResult { Answered, TimedOut, Interrupted, ConnectionLost };
  HealthProbeResult ProbeHostHealth(uint64_t request_id,
                                    std::optional<ipc::HealthPayload>* payload);
  enum class DiagnosticsProbeResult { Answered, Failed, Interrupted, ConnectionLost };
  DiagnosticsProbeResult ProbeHostDiagnostics(uint64_t request_id);
  void PublishHealthDisplayState(CandidateHealthState state);
  void NoteHostResponded();
  void NoteHostDeadlineMissed();
  bool HasQueuedIpcWorkLocked() const;
  bool WaitForIpcResponseOrStop(uint32_t timeout_ms, uint64_t expected_request_id,
                                ipc::MessageType expected_type,
                                std::optional<ipc::Envelope>* received = nullptr);
  bool ObserveHostGeneration(const std::string& host_generation_id);
  void RequestHostOptionRefresh();
  void RetryModelFromUi();
  void RetryModelOnControlThread();
  void RearmPendingQuery(uint64_t req_id);
  void RequeueUnackedSendItems(std::vector<IpcSendItem>& items, size_t from_index);
  void TrimIpcSendQueueLocked();
  void CancelInflightWindowQueryLocked();
  std::string NextCommitObservationId();
  std::string CurrentActionTraceId() const;
  void PostQueryCandidates(ITfContext* context, const std::string& reading, bool live = true,
                           const std::string& emoji_trigger = {});
  void PostQueryLiveConversion(ITfContext* context, const std::string& reading,
                               LiveRequestTiming timing = LiveRequestTiming::Edit);
  // M59 dynamic punctuation (docs/dynamic-punctuation-spec.md sections 4.3.1, 7.3, 7.4).
  void ArmIdlePunctuationTimer();
  void OnPunctuationTimer(DelayedActionTimer::Slot slot);
  static void PunctuationTimerTrampoline(void* context, DelayedActionTimer::Slot slot) noexcept;
  bool BeginFinalPunctuationHold(ITfContext* context, const core::InputState& previous,
                                 core::HandleResult& result);
  void ResolveFinalPunctuationHold();
  void AbortFinalPunctuationHold();
  void CancelPunctuationTimers();
  void AdoptLiveCommitObservation(ITfContext* context);
  void AdoptLiveSegmentsObservation(ITfContext* context,
                                    const std::vector<ipc::LiveSegment>& segments,
                                    const std::string& surface);
  bool PreparePunctuationConversion(const std::string& reading);
  bool BeginPunctuationConversionHold(ITfContext* context, const core::UserActionEvent& event,
                                      const std::string& reading);
  void ResolvePunctuationConversionHoldIfReady();
  void ObservePunctuationConversion(ITfContext* context, const core::ObserveCommit& observe);
  std::optional<uint64_t> ApplyLiveConversionResultImpl();
  std::string CaptureLeftContext(ITfContext* context, bool secure);
  HRESULT HandleEmojiKey(ITfContext* context, WPARAM key, LPARAM key_data, BOOL* eaten,
                         bool test_only, bool& handled);
  void PostBatchConversion(const std::string& reading, const std::string& raw_romaji,
                           ITfContext* context);
  // Single evaluation point for the M46 secure decision and the AI privacy
  // axes. Owner thread only: it probes the foreground app and the TSF input
  // scope. Updates secure_input_ so every outbound route sees one answer.
  struct PrivacyDecision {
    bool secure{false};
    bool learning_allowed{false};
    bool prediction_allowed{false};
    core::AiPrivacy ai;
    std::string backend;
    bool detailed_logging_allowed{false};
  };
  PrivacyDecision ResolvePrivacy(ITfContext* context, bool evaluate_ai,
                                 std::optional<TfEditCookie> edit_cookie = std::nullopt);
  static void OnCandidatesReady(void* context);
  bool ShowCandidateWindowFromCache();
  void ApplyLiveConversionResult();
  void ApplyPredictionResult();
  void RefreshPrediction(ITfContext* context);
  void ClearPrediction();
  HRESULT AcceptPrediction(ITfContext* context, size_t index);
  HRESULT ApplyPredictionReading(ITfContext* context, const std::string& reading);
  RECT PredictionCaretRect();
  // `suggestions` come only from a batch correction select (rich-features-spec
  // X-3-6); every other caller opens Host candidates alone.
  HRESULT StartSelectionReconversion(ITfContext* context,
                                     const std::wstring* expected_surface = nullptr,
                                     ITfRange* expected_range = nullptr,
                                     std::vector<std::wstring> suggestions = {});
  HRESULT CompleteReconversionSelection(size_t index);
  // A left click on candidate row `idx`, from the candidate window.
  void OnCandidateClicked(int idx);
  void ClearReconversionState();
  void ShowReconversionResult();
  // Opens the list for `range` if it is still the open request's.
  void ShowReconversionList(uint64_t generation, ITfRange* range, HRESULT hr,
                            ITfCandidateList* list, std::vector<std::wstring> values,
                            std::vector<ReconversionOrigin> origins, std::string reading);
  void PrefetchSelectedReconversion();
  void AdviseTextEditSink(ITfContext* context);
  void UnadviseTextEditSink();
  HRESULT ApplyClientAction(ITfContext* context, const core::ClientAction& action,
                            bool& preedit_update_needed, bool& commit_pending);
  HRESULT ApplyInputStateResult(ITfContext* context, core::HandleResult result);
  std::vector<core::Candidate> CoreCandidatesFromCache();
  POINT CandidateAnchorPoint();
  std::string CurrentPreeditSurface() const;
  std::string CurrentDisplayedPreeditSurface() const;
  std::optional<std::string> LiveShownSurface(const std::string& reading,
                                              bool require_setting = true) const;
  // Batch segments as the preedit shows them, in order; empty when the preedit
  // is not a batch conversion with the candidate window open.
  std::vector<DisplayedSegment> DisplayedBatchSegments() const;
  bool ShowsBatchSegments() const;
  // The candidate surface the segment shows, or its reading when none is chosen.
  const std::string& DisplayedBatchSegmentSurface(size_t index, bool* converted) const;
  // Moves the batch-conversion focus to the segment and re-shows its candidates.
  HRESULT FocusBatchSegment(size_t index);
  // Registers display_attribute_atoms_ once; false while any atom is missing.
  bool EnsureDisplayAttributeAtoms();
  void AdviseCompositionMouseSink(ITfContext* context, ITfRange* range);
  void UnadviseCompositionMouseSink();
  bool BatchRomajiEnabled() const;
  // M60: inlineEnglishCandidates is on and the Host advertised
  // "english_candidates" (docs/inline-english-candidate-spec.md §6.2, §7).
  bool EnglishCandidatesEnabled() const;
  // The typed romaji when it still spells `reading`, empty otherwise.
  std::string EnglishRawRomajiFor(const std::string& reading) const;
  // M63 (docs/mixed-english-auto-conversion-spec.md section 5): the raw romaji
  // to send with mixed_english for `reading`, or empty when any condition of
  // section 5 fails. `secure` is the request's privacy decision.
  std::string MixedEnglishRawRomajiFor(const std::string& reading, bool secure) const;
  // DEV-1734 (docs/legacy-parity-spec.md section 5.4): the other readings of the
  // typed romaji's first `max_ambiguous` "nn" + vowel positions, for
  // alt_readings. Empty with a custom table, or when the romaji no longer
  // spells `reading`.
  std::vector<std::string> AltReadingsFor(const std::string& reading, size_t max_ambiguous) const;
  // The window compares two readings at most: the Host converts each with the
  // model within the window budget.
  static constexpr size_t kWindowAmbiguousPositions = 1;
  // DEV-1734: the reading a commit is learned under. A candidate of the other
  // reading of an ambiguous "nn" + vowel keeps its own reading, as an M35
  // correction is learned under the corrected reading the preedit then holds.
  std::string ObservedReading(const std::string& reading, const ipc::CandidateField& chosen) const;
  // Drops trailing romaji until the rest spells the current preedit, so a
  // Backspace that erased a whole kana also erases the keys that typed it.
  void SyncEnglishRawRomaji();
  std::string BatchPreviewSurface() const;
  std::string BatchReadingForConversion() const;
  void RefreshBatchPreeditSurface();
  void ClearBatchState();
  enum class LifecycleCleanupFailurePolicy {
    PreserveComposition,
    ReleaseComposition,
  };
  void ClearCandidateStateForLifecycle();
  void CancelPendingQueriesForLifecycle();
  void SetCommitContext(ITfContext* context);
  void ClearCommitContext();
  void PostPendingCommitObservation();
  void TrackTypoKey(ITfContext* context, WPARAM key);
  // Consumes the one-key undo window; a plain Backspace in it sends "undo".
  void TrackUndoKey(ITfContext* context, WPARAM key);
  void PostCommitCorrection(ITfContext* context, std::string_view kind, std::string reading,
                            std::string rejected_surface,
                            std::optional<std::string> selected_surface);
  void PrepareTypoCommit(const std::string& reading);
  void CompleteTypoCommit(std::optional<PendingTypoCommit> pending, bool committed) noexcept;
  void ResetTypoTracking();
  HRESULT ApplyPendingCorrectedReading(ITfContext* context);
  bool CanSendQueuedItem(const IpcSendItem& item) const;
  void ClearTextStateForLifecycle();
  bool ActiveContextBelongsToDocumentMgr(ITfDocumentMgr* document_mgr) const;
  HRESULT RequestCommitEditSession(ITfContext* context);
  bool RequestLifecycleCommitOrEndComposition(ITfContext* context,
                                              const std::string& shown_surface);
  std::string LifecycleShownSurface() const;
  void HoldRetryWaitingKey(ITfContext* context, WPARAM key, LPARAM key_data);
  // DEV-1813 (legacy-parity-spec §1.2): what a key does while the candidate
  // window waits for its answer after Space.
  enum class WindowWaitKey { Hold, Drop, Pass };
  bool AwaitingCandidateWindow();
  HRESULT OpenLocalCandidateWindow(ITfContext* context);
  WindowWaitKey ClassifyWindowWaitKey(WPARAM key, LPARAM key_data, uint32_t modifiers,
                                      OpenCloseKeyAction open_close);
  void HoldWindowWaitingKey(ITfContext* context, WPARAM key, LPARAM key_data, bool tracked = true);
  // DEV-1794: a character key that waits with the held Enter (section 7.3).
  bool HoldsKeyForFinalPunctuation(ITfContext* context, WPARAM key, LPARAM key_data);
  HRESULT ReplayWindowWaitingKeys(ITfContext* context);
  void ReplayWindowWaitingKeysIfReady();
  HRESULT ReplayRetryWaitingKeys(ITfContext* context, WPARAM retry_key, LPARAM retry_key_data);
  HRESULT AdviseKeyboardOpenCompartment();
  HRESULT UnadviseKeyboardOpenCompartment();
  HRESULT AdviseFunctionProvider();
  HRESULT UnadviseFunctionProvider();
  void RefreshKeyboardOpen(std::string_view source);
  void UpdateInputModeLangBar();
  void OnInputModeLangBarClick(TfLBIClick click, POINT point, const RECT* area);
  void ShowInputModeLangBarMenu(POINT point, const RECT* area);
  void RunInputModeMenuCommand(InputModeMenuCommand command, const InputModeMenuState& shown);
  void ToggleSettingFromMenu(std::string_view key, bool value);
  HRESULT HandleOpenCloseKey(OpenCloseKeyAction action, WPARAM key, ITfContext* context);
  bool HasInputToCommitBeforeClose() const;
  // Spec §3.7: whether this Space, with no reading in hiragana mode, inserts
  // U+3000 instead of reaching the app.
  bool InsertsFullWidthSpace(ITfContext* context, WPARAM key, uint32_t modifiers) const;
  // Inserts U+3000 at the selection without a composition. Unless this is the
  // retry, a lock not granted keeps the context in full_width_space_context_;
  // any other failure drops the space.
  HRESULT InsertFullWidthSpace(ITfContext* context, bool retry);
  // Inserts a waiting space once before the key from `context`, or drops it
  // when the key comes from another context.
  HRESULT ResolvePendingFullWidthSpace(ITfContext* context);
  HRESULT CloseKeyboardCommittingShownText(ITfContext* context, std::string_view source);
  void OpenKeyboardIfClosed(std::string_view source);
  void ApplyInitialKeyboardOpenOnFocus(ITfDocumentMgr* document_mgr);
  void ReopenIfInitialOpenWasUndone();
  void ClearKeyboardOpenTracking();
  void LogKeyPassthroughOnce(bool keyboard_disabled = false) noexcept;
  bool KeyPathLogging() const noexcept;
  void LogKeyPath(std::string_view sink, ITfContext* context, WPARAM key, BOOL eaten) noexcept;
  void LogKeyPathContext(std::string_view operation, ITfContext* context) noexcept;
  void LogInputGateOnChange(const InputGateDecision& gate, bool secure) noexcept;
  void LogPredictionOnce(PredictionLogReason reason) noexcept;
  void ShowPredictionWindow();
  void LogKeyboardOpenState(std::string_view event, std::string_view source,
                            bool open_before) const noexcept;
  void CleanupForLifecycleLoss(ITfContext* context, bool release_active_context,
                               LifecycleCleanupFailurePolicy failure_policy,
                               bool keep_unshown_input = false);
  bool CanCarryUnshownInput(ITfDocumentMgr* focus) const;
  void ResolveCarriedInput(ITfContext* context);
  void ShowCarriedInput(ITfDocumentMgr* focus);
  bool CarriesUnshownInput();
  void DropCarriedInput();

  // M6: enqueue a CommitObservation to the IPC worker.
  void PostCommitObservation(const std::string& reading, const ipc::CandidateField& chosen,
                             const std::vector<ipc::CandidateField>& shown, bool secure,
                             bool learning_allowed);
  // M10: enqueue a Cancel message to the IPC worker.
  void PostCancel(uint64_t target_request_id);
  // Internal: push an item onto ipc_send_queue_ and notify the worker.
  void PostIpcSend(ipc::MessageType type, std::string payload, bool expects_response);

  // M18 (docs/legacy-parity-spec.md §6-§8).
  void ApplyPostCommitActions(ITfContext* context, const std::vector<core::ClientAction>& actions);
  HRESULT FinishUnicodeInputBeforeKey(ITfContext* context, WPARAM key, uint32_t modifiers);
  bool UnicodeInputActive() const;
  // Single policy point for whether the TIP takes F10 (spec §8.1).
  bool ClaimsDebugWindowKey(ITfContext* context) const;
  // True when the key enters or edits the core Unicode input state; such keys
  // always take the core path, whatever the legacy pre-processing settings.
  bool UnicodeInputRouted(const std::optional<core::UserActionEvent>& key_event, bool has_preedit,
                          bool cand_visible) const;
  void PlayBeep();
  void ToggleDebugWindow();
  // True when Ctrl+Shift+Backspace has a pair to forget here. A secure context
  // drops the record and leaves the key to the application (M46).
  bool ClaimsForgetKey(ITfContext* context);
  void ForgetLastCommit(ITfContext* context);
  void RecordDebugIpc(DebugIpcLogEntry entry, bool secure, bool detailed_logging_allowed) noexcept;
  void RegisterDebugPreservedKey();
  void UnregisterDebugPreservedKey();
  // Ctrl+Shift+Space (rich-features-spec X-3-6). Taken only with the IME open
  // in kana mode, no reading in progress, and a context that accepts keys.
  bool ClaimsProofreadKey(ITfContext* context) const;
  ProofreadCapture CaptureProofreadDocument(ITfContext* context);
  // Replaces any session: captures the document, serves it on a new session
  // pipe and opens the settings app on it.
  void StartProofreadSession(ITfContext* context);
  void EndProofreadSession();
  static void OnProofreadSelect(void* context, ipc::proofread::SelectRequest request) noexcept;
  static void OnProofreadEnd(void* context) noexcept;
  // With `request.reconvert`, a placed selection also opens reconversion candidates.
  void ApplyProofreadSelection(ipc::proofread::SelectRequest request);
  void CompleteProofreadSelection(uint64_t generation, ipc::proofread::SelectResult result);
  void QueueProofreadReconversion(uint64_t generation, ITfContext* context, ITfRange* range,
                                  std::wstring surface, std::vector<std::wstring> suggestions);
  void RunPendingProofreadReconversion();
  friend class ProofreadSelectSession;
  void RegisterProofreadPreservedKey();
  void UnregisterProofreadPreservedKey();

  // M5 commit helpers.
  HRESULT CommitSelected(ITfContext* context);
  HRESULT CommitPreeditAsIs(ITfContext* context);
  // F6-F10 (tsf-deep-integration-spec §3.6): commit the whole reading in one form.
  HRESULT CommitReadingInForm(ITfContext* context, core::CommitForm form);
};

class EditSession final : public ITfEditSession {
 public:
  EditSession(TextService* service, ITfContext* context);

  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;
  STDMETHODIMP DoEditSession(TfEditCookie ec) override;

 private:
  LONG ref_count_{1};
  TextService* service_;
  ITfContext* context_;
};

}  // namespace azookey::tsf
