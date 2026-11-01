#include "azookey/host/Dispatcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <string_view>

#include "azookey/host/AnomalyDetector.h"
#include "azookey/host/HealthStateMachine.h"
#include "azookey/host/ModelBenchmark.h"
#include "azookey/host/ModelScanner.h"
#include "azookey/host/PunctuationInserter.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Psapi.h requires the Windows SDK types declared by Windows.h.
// clang-format off
#include <Windows.h>
#include <Psapi.h>
// clang-format on
#endif

#include "azookey/core/BatchConversionChunker.h"
#include "azookey/core/CrashReporting.h"
#include "azookey/core/EtwLogger.h"
#include "azookey/core/MixedRomajiSplitter.h"
#include "azookey/core/PlatformPaths.h"
#include "azookey/core/RomajiKanaConverter.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/ipc/TraceId.h"
#include "azookey/logging/Phase.h"

namespace azookey::host {

namespace {

constexpr std::int32_t kGenericFailureHresult = static_cast<std::int32_t>(0x80004005u);

logging::RuntimeLogger& DispatcherLogger(logging::RuntimeLogger* configured) {
  if (configured) return *configured;
  static logging::RuntimeLogger fallback(logging::RuntimeLoggerOptionsFromEnvironment("host"));
  return fallback;
}

const char* AiErrorClassToWire(AiErrorClass error) {
  switch (error) {
    case AiErrorClass::Auth:
      return "Auth";
    case AiErrorClass::RateLimit:
      return "RateLimit";
    case AiErrorClass::ServerError:
      return "ServerError";
    case AiErrorClass::Network:
      return "Network";
    case AiErrorClass::Timeout:
      return "Timeout";
    case AiErrorClass::Parse:
      return "Parse";
    case AiErrorClass::BlockedBySecure:
      return "BlockedBySecure";
    case AiErrorClass::Disabled:
      return "Disabled";
    case AiErrorClass::Canceled:
      return "Canceled";
    case AiErrorClass::KeyReentry:
      return "KeyReentry";
    case AiErrorClass::None:
      return nullptr;
  }
  return nullptr;
}

core::EtwGuid ClientGuid(std::string_view value) {
  core::EtwGuid result{};
  if (value.size() == 38 && value.front() == '{' && value.back() == '}')
    value = value.substr(1, 36);
  if (value.size() != 36) return result;
  const auto hex = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  // GUID uses little endian Data1/Data2/Data3, then eight byte-sized fields.
  constexpr size_t positions[]{6, 4, 2, 0, 11, 9, 16, 14, 19, 21, 24, 26, 28, 30, 32, 34};
  if (value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-') return result;
  for (size_t index = 0; index < result.size(); ++index) {
    const int high = hex(value[positions[index]]);
    const int low = hex(value[positions[index] + 1]);
    if (high < 0 || low < 0) return {};
    result[index] = static_cast<uint8_t>(high * 16 + low);
  }
  return result;
}

class InferenceTrace {
 public:
  InferenceTrace(uint64_t request, std::string_view client, std::string_view trace_id,
                 core::EtwBackend backend, uint64_t length, logging::RuntimeLogger* logger)
      : context_{request, ClientGuid(client), trace_id}, logger_(logger) {
    core::EtwLogger::LogInferenceStart(request, backend, length, context_.client);
  }
  ~InferenceTrace() {
    if (result_ == core::EtwResult::Failed && cancel_ && cancel_->load(std::memory_order_acquire))
      result_ = core::EtwResult::Cancelled;
    const double latency_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_)
            .count();
    core::EtwLogger::LogInferenceEnd(context_.request_id, candidates_, latency_ms, result_,
                                     context_.client);
    if (!logger_ || !logger_->enabled()) return;
    try {
      if (result_ == core::EtwResult::Success || result_ == core::EtwResult::Cancelled) {
        logger_->Log(logging::RuntimeLogLevel::Info, "query_latency",
                     {{"trace_id", logging::RuntimeLogSafeText(std::string(context_.trace_id))},
                      {"request_id", context_.request_id},
                      {"latency_ms", latency_ms},
                      {"result", logging::RuntimeLogSafeText(
                                     result_ == core::EtwResult::Success ? "ok" : "cancelled")}});
      } else {
        logger_->Log(
            logging::RuntimeLogLevel::Error, "query_latency",
            {{"trace_id", logging::RuntimeLogSafeText(std::string(context_.trace_id))},
             {"request_id", context_.request_id},
             {"latency_ms", latency_ms},
             {"result", logging::RuntimeLogSafeText("error")},
             {"error_code", logging::RuntimeLogSafeText(error_code_ == core::EtwErrorCode::Protocol
                                                            ? "protocol"
                                                            : "business")}});
      }
    } catch (...) {
      // Telemetry allocation must not terminate request completion.
    }
  }
  void Finish(core::EtwResult result, uint64_t candidates = 0) {
    result_ = result;
    candidates_ = candidates;
  }
  void ProtocolError() { error_code_ = core::EtwErrorCode::Protocol; }
  const InferenceTelemetry* context() const { return &context_; }
  void WatchCancellation(std::shared_ptr<std::atomic<bool>> cancel) { cancel_ = std::move(cancel); }

 private:
  InferenceTelemetry context_;
  std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
  core::EtwResult result_{core::EtwResult::Failed};
  core::EtwErrorCode error_code_{core::EtwErrorCode::Business};
  uint64_t candidates_{};
  std::shared_ptr<std::atomic<bool>> cancel_;
  logging::RuntimeLogger* logger_;
};

uint64_t NowMs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

uint64_t NowSec() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

uint64_t CurrentRssMb() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    return static_cast<uint64_t>(counters.WorkingSetSize / (1024ULL * 1024ULL));
  }
#endif
  return 0;
}

ipc::Envelope MakeResponse(const ipc::Envelope& req, std::string payload_json) {
  ipc::Envelope r;
  r.version = req.version;
  r.request_id = req.request_id;
  r.trace_id = req.trace_id;
  r.type = req.type;
  r.payload_json = std::move(payload_json);
  return r;
}

const char* SourceToWire(core::CandidateSource source) {
  if (source == core::CandidateSource::SystemDictionary) return "system";
  if (source == core::CandidateSource::UserDictionary) return "user_dict";
  if (source == core::CandidateSource::Model) return "model";
  if (source == core::CandidateSource::Llm) return "llm";
  if (source == core::CandidateSource::Learning) return "learning";
  if (source == core::CandidateSource::Symbol) return "symbol";
  if (source == core::CandidateSource::Emoji) return "emoji";
  return "heuristic";
}

ipc::CandidateField ToField(const core::Candidate& c) {
  ipc::CandidateField f;
  f.surface = c.surface;
  f.reading = c.reading;
  f.score = c.score;
  f.source = SourceToWire(c.source);
  f.description = c.description;
  f.tag = static_cast<uint8_t>(c.tag);
  return f;
}

// docs/app-profile-spec.md section 3: an absent or unresolved app gets no
// profile and no tag boost at all, not even the "default" profile's.
TagBoosts ResolveTagBoosts(const EngineConfig& config, const std::optional<ipc::AppIdentity>& app) {
  if (!config.app_profiles || !app || app->process_name.empty()) return {};
  core::ForegroundApp foreground;
  foreground.process_name = app->process_name;
  foreground.window_class = app->window_class;
  foreground.resolved = true;
  return TagBoostsFromProfile(config.app_profiles->Resolve(foreground));
}

std::optional<BackendKind> ParseBackend(const std::string& backend) {
  if (backend.empty() || backend == "cpu") return BackendKind::Cpu;
  if (backend == "cuda") return BackendKind::Cuda;
  if (backend == "vulkan") return BackendKind::Vulkan;
  return std::nullopt;
}

bool ConstantTimeEquals(std::string_view left, std::string_view right) {
  const size_t compared_size = std::max(left.size(), right.size());
  volatile size_t difference = left.size() ^ right.size();
  for (size_t i = 0; i < compared_size; ++i) {
    const auto left_byte = i < left.size() ? static_cast<unsigned char>(left[i]) : 0;
    const auto right_byte = i < right.size() ? static_cast<unsigned char>(right[i]) : 0;
    difference = difference | (left_byte ^ right_byte);
  }
  return difference == 0;
}

class RequestCompletionGuard {
 public:
  RequestCompletionGuard(RequestScheduler* scheduler, std::string client_id, uint64_t request_id)
      : scheduler_(scheduler), client_id_(std::move(client_id)), request_id_(request_id) {}

  ~RequestCompletionGuard() { Complete(); }

  void Complete() {
    if (active_) {
      scheduler_->CompleteRequest(client_id_, request_id_);
      active_ = false;
    }
  }

 private:
  RequestScheduler* scheduler_;
  std::string client_id_;
  uint64_t request_id_;
  bool active_{true};
};

void LogHostQueueWait(logging::RuntimeLogger* logger, const ipc::Envelope& req,
                      std::optional<std::chrono::steady_clock::time_point> start,
                      std::string_view result) {
  if (!start || !ipc::IsValidTraceId(req.trace_id)) return;
  // RequestScheduler has no work queue. This measures admission, including any
  // wait on its mutex, rather than claiming time spent before transport dispatch.
  logger->Log(logging::RuntimeLogLevel::Info, "trace_phase",
              {{"trace_id", logging::RuntimeLogSafeText(req.trace_id)},
               {"request_id", req.request_id},
               {"phase", logging::RuntimeLogSafeText(
                             std::string(logging::PhaseName(logging::Phase::HostQueueWait)))},
               {"latency_ms",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *start)
                    .count()},
               {"result", logging::RuntimeLogSafeText(std::string(result))}});
}

// DEV-1581: the AddUserWord / RemoveUserWord `error` is the edit result's name.
constexpr bool UserDictErrorMatches(UserDictionaryEditResult result, std::string_view code) {
  return std::string_view(UserDictionaryEditResultName(result)) == code;
}
static_assert(UserDictErrorMatches(UserDictionaryEditResult::Busy, ipc::kUserDictErrorBusy));
static_assert(UserDictErrorMatches(UserDictionaryEditResult::LockFailed,
                                   ipc::kUserDictErrorLockFailed));
static_assert(UserDictErrorMatches(UserDictionaryEditResult::LoadFailed,
                                   ipc::kUserDictErrorLoadFailed));
static_assert(UserDictErrorMatches(UserDictionaryEditResult::SaveFailed,
                                   ipc::kUserDictErrorSaveFailed));
static_assert(UserDictErrorMatches(UserDictionaryEditResult::NotFound,
                                   ipc::kUserDictErrorNotFound));
static_assert(UserDictErrorMatches(UserDictionaryEditResult::Unavailable,
                                   ipc::kUserDictErrorUnavailable));

// DEV-1572: one record per user dictionary or new-word edit, so a CLI failure
// can be matched with what the Host was waiting for. Never the word itself.
// The receipt is the record's timestamp minus total_ms.
void LogPersistentEdit(logging::RuntimeLogger* logger, const ipc::Envelope& req,
                       std::string_view op, const UserDictionaryEditReport& report) {
  if (!logger || !logger->enabled()) return;
  const bool failed = report.result != UserDictionaryEditResult::Ok &&
                      report.result != UserDictionaryEditResult::NotFound;
  logger->Log(failed ? logging::RuntimeLogLevel::Warn : logging::RuntimeLogLevel::Info,
              "persistent_edit",
              {{"trace_id",
                logging::RuntimeLogSafeText(ipc::IsValidTraceId(req.trace_id) ? req.trace_id : "")},
               {"request_id", req.request_id},
               {"op", logging::RuntimeLogSafeText(std::string(op))},
               {"result", logging::RuntimeLogSafeText(UserDictionaryEditResultName(report.result))},
               {"writer_wait_ms", report.writer_wait_ms},
               {"lock_wait_ms", report.lock_wait_ms},
               {"load_ms", report.load_ms},
               {"load_skipped", report.load_skipped},
               {"save_ms", report.save_ms},
               {"total_ms", report.total_ms}});
}

// M59 (dynamic-punctuation-spec section 7): the live conversion surface as one
// converted segment, with punctuation inserted by the configured rules.
// nullopt when the request or the settings leave punctuation off.
std::optional<PunctuationResult> InsertLivePunctuation(const EngineConfig& config,
                                                       bool auto_punctuation,
                                                       const std::string& style,
                                                       const std::string& surface,
                                                       const std::string& reading) {
  if (!auto_punctuation || !config.enable_live_conversion || !config.dynamic_punctuation ||
      surface.empty()) {
    return std::nullopt;
  }
  ipc::LiveSegment converted;
  converted.surface = surface;
  converted.reading = reading;
  converted.score = 1.0;
  const auto rules = PunctuationInserter::LoadRules(config.punctuation_rules_path);
  return PunctuationInserter::Insert({converted}, rules, style, config.segment_boundary_confidence);
}

// M63 (docs/mixed-english-auto-conversion-spec.md section 6): the split the
// Host adopts, or nullopt to handle the request as with M63 off (section 10).
// With `alternatives`, also the readable words of its J segments that section
// 8.3 item 3 turns English one at a time, from the same dictionary snapshot.
std::optional<core::MixedRomajiSplit> SplitComposition(
    InferenceEngine& engine, const std::string& raw_romaji, bool inline_high_confidence,
    std::vector<core::MixedRomajiAlternative>* alternatives = nullptr) {
  core::MixedRomajiSplitOptions options;
  options.min_english_length = engine.config().english.min_length;
  options.split_inline = true;
  options.inline_high_confidence = inline_high_confidence;
  std::vector<core::MixedRomajiSplit> splits;
  // One dictionary snapshot for every substring of the lattice. The split is
  // pure, so a retried snapshot only repeats it; Zenzai runs after it returns.
  engine.WithEnglishDictionary([&](const EnglishDictionary* dictionary) {
    core::EnglishWordLookup lookup;
    if (dictionary) {
      lookup = [dictionary](std::string_view lower_key) {
        std::vector<core::EnglishWordMatch> matches;
        for (auto& entry : dictionary->Lookup(lower_key))
          matches.push_back({std::move(entry.surface), entry.flags});
        return matches;
      };
    }
    splits = core::SplitMixedRomaji(raw_romaji, lookup, options);
    if (alternatives && !splits.empty())
      *alternatives =
          core::ReadableEnglishAlternatives(raw_romaji, splits.front(), lookup, options);
  });
  if (splits.empty()) return std::nullopt;
  // Section 8.1: a sure segment typed without capitals (so sure by (ii)) whose
  // key the user has turned down (section 11) is low confidence, for the whole
  // composition too. Looked up after the dictionary snapshot is released.
  const auto suppress = [&](core::MixedRomajiSplit& split) {
    for (auto& segment : split.segments) {
      if (segment.kind != core::MixedRomajiSegmentKind::English ||
          segment.confidence != core::EnglishSegmentConfidence::High)
        continue;
      const auto typed = std::string_view(raw_romaji)
                             .substr(segment.raw_begin, segment.raw_end - segment.raw_begin);
      if (std::any_of(typed.begin(), typed.end(), [](char c) { return c >= 'A' && c <= 'Z'; }))
        continue;
      if (engine.IsEnglishSuppressed(EnglishLookupKey(typed)))
        segment.confidence = core::EnglishSegmentConfidence::Low;
    }
  };
  suppress(splits.front());
  if (alternatives)
    for (auto& alternative : *alternatives) suppress(alternative.split);
  return std::move(splits.front());
}

// M63-A (section 8.2): the whole composition as one English segment, or
// nullopt (a J segment, or a split of section 8.3).
std::optional<core::MixedRomajiSegment> WholeCompositionEnglish(
    const std::optional<core::MixedRomajiSplit>& split) {
  if (!split || split->segments.size() != 1) return std::nullopt;
  const auto& segment = split->segments.front();
  if (segment.kind != core::MixedRomajiSegmentKind::English) return std::nullopt;
  return segment;
}

// M63-B (section 8.3): a split of the composition into J and E segments.
bool IsInlineSplit(const std::optional<core::MixedRomajiSplit>& split) {
  return split && split->segments.size() > 1;
}

// Section 7: the reading Zenzai converts when the E segments flagged in
// `english` stay English. The other segments join the J runs around them; a
// run's kana is the romaji table's kana of the whole run.
struct MixedReading {
  struct Part {
    bool english{};
    size_t raw_begin{};
    size_t raw_end{};
    std::string text;  // The English surface, or the run's kana.
  };
  std::vector<Part> parts;
  std::string reading;
  std::vector<std::pair<size_t, size_t>> english_ranges;
};

MixedReading BuildMixedReading(const core::MixedRomajiSplit& split, const std::string& raw_romaji,
                               const std::vector<bool>& english) {
  MixedReading mixed;
  for (size_t i = 0; i < split.segments.size(); ++i) {
    const auto& segment = split.segments[i];
    if (english[i]) {
      mixed.parts.push_back({true, segment.raw_begin, segment.raw_end, segment.surface});
    } else if (!mixed.parts.empty() && !mixed.parts.back().english) {
      mixed.parts.back().raw_end = segment.raw_end;
    } else {
      mixed.parts.push_back({false, segment.raw_begin, segment.raw_end, {}});
    }
  }
  for (auto& part : mixed.parts) {
    if (!part.english) {
      std::string lower = raw_romaji.substr(part.raw_begin, part.raw_end - part.raw_begin);
      for (auto& c : lower)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      part.text = core::RomajiKanaConverter::ConvertForCommit(lower);
    } else {
      mixed.english_ranges.emplace_back(mixed.reading.size(),
                                        mixed.reading.size() + part.text.size());
    }
    mixed.reading += part.text;
  }
  return mixed;
}

// Section 9.2: the segments of a conversion of a mixed reading, or nullopt
// when the surface does not hold every English surface in order with
// non-empty Japanese text between them (the model changed or dropped one).
std::optional<std::vector<ipc::LiveSegment>> SegmentsOf(const std::string& surface,
                                                        const MixedReading& mixed,
                                                        const std::string& raw_romaji,
                                                        double score) {
  std::vector<ipc::LiveSegment> segments;
  uint32_t offset = 0;
  const auto push = [&](std::string text, std::string reading, bool english) {
    ipc::LiveSegment segment;
    segment.start_char = offset;
    offset += Utf16Length(text);
    segment.end_char = offset;
    segment.score = score;
    segment.surface = std::move(text);
    segment.reading = std::move(reading);
    if (english) segment.tag = static_cast<uint8_t>(core::CandidateTag::English);
    segments.push_back(std::move(segment));
  };
  size_t cursor = 0;
  for (size_t i = 0; i < mixed.parts.size(); ++i) {
    const auto& part = mixed.parts[i];
    if (!part.english) {
      if (i + 1 < mixed.parts.size()) continue;  // Closed by the next E part.
      if (cursor >= surface.size()) return std::nullopt;
      push(surface.substr(cursor), part.text, false);
      cursor = surface.size();
      continue;
    }
    const auto found = surface.find(part.text, cursor);
    if (found == std::string::npos) return std::nullopt;
    const bool japanese_before = i > 0 && !mixed.parts[i - 1].english;
    if (japanese_before != (found > cursor)) return std::nullopt;
    if (japanese_before)
      push(surface.substr(cursor, found - cursor), mixed.parts[i - 1].text, false);
    push(part.text, raw_romaji.substr(part.raw_begin, part.raw_end - part.raw_begin), true);
    cursor = found + part.text.size();
  }
  if (cursor != surface.size()) return std::nullopt;
  return segments;
}

// Section 8.3, items 1 to 3, in order; empty when none of them differs from
// the list M63 off returns. all_japanese is the first candidate of the
// conversion of the TIP's kana (item 2, or item 1 when no segment is sure);
// nullopt leaves it out, for a split of one J segment whose list keeps the
// first candidate of M63 off. Item 3 turns the low-confidence E segments
// English first, then the readable words of `alternatives`.
std::vector<ipc::CandidateField> MixedCandidates(
    InferenceEngine& engine, const ipc::QueryCandidatesRequest& request,
    const core::MixedRomajiSplit& split,
    const std::vector<core::MixedRomajiAlternative>& alternatives,
    const std::optional<core::Candidate>& all_japanese,
    std::chrono::steady_clock::time_point deadline, const std::atomic<bool>* cancel,
    const InferenceTelemetry* telemetry, const TagBoosts* tag_boosts) {
  constexpr size_t kMaxLowConfidenceConversions = 3;
  std::vector<bool> english(split.segments.size());
  std::vector<size_t> low;
  for (size_t i = 0; i < split.segments.size(); ++i) {
    const auto& segment = split.segments[i];
    if (segment.kind != core::MixedRomajiSegmentKind::English) continue;
    if (segment.confidence == core::EnglishSegmentConfidence::High)
      english[i] = true;
    else
      low.push_back(i);
  }
  // Section 10: no conversion starts after the deadline, a running one ends by
  // it, and one it or the Zenzai budget cut short is left out.
  const auto convert = [&](const core::MixedRomajiSplit& from,
                           const std::vector<bool>& flags) -> std::optional<ipc::CandidateField> {
    if (std::chrono::steady_clock::now() >= deadline ||
        (cancel && cancel->load(std::memory_order_acquire)))
      return std::nullopt;
    const auto mixed = BuildMixedReading(from, request.raw_romaji, flags);
    MixedConversionOptions options;
    options.english_ranges = mixed.english_ranges;
    options.deadline = deadline;
    options.typo_correction = false;
    options.nll = false;
    auto result =
        engine.QueryCandidatesEx(mixed.reading, request.left_context, NowSec(), cancel,
                                 request.max_candidates, false, telemetry, tag_boosts, &options);
    if (result.model_deadline_exceeded || std::chrono::steady_clock::now() >= deadline)
      return std::nullopt;
    for (const auto& candidate : result.candidates) {
      auto segments = SegmentsOf(candidate.surface, mixed, request.raw_romaji, candidate.score);
      if (!segments) continue;
      auto field = ToField(candidate);
      field.reading = request.reading;
      field.tag = static_cast<uint8_t>(core::CandidateTag::None);
      field.segments = std::move(*segments);
      return field;
    }
    return std::nullopt;
  };
  std::vector<ipc::CandidateField> out;
  if (std::find(english.begin(), english.end(), true) != english.end())
    if (auto first = convert(split, english)) out.push_back(std::move(*first));
  const bool mixed_first = !out.empty();
  if (all_japanese) {
    auto field = ToField(*all_japanese);
    field.reading = request.reading;
    field.tag = static_cast<uint8_t>(core::CandidateTag::None);
    ipc::LiveSegment whole;
    whole.end_char = Utf16Length(field.surface);
    whole.score = field.score;
    whole.surface = field.surface;
    whole.reading = request.reading;
    field.segments = {std::move(whole)};
    out.push_back(std::move(field));
  }
  size_t flipped = 0;
  for (size_t index : low) {
    if (flipped++ == kMaxLowConfidenceConversions) break;
    auto flags = english;
    flags[index] = true;
    if (auto candidate = convert(split, flags)) out.push_back(std::move(*candidate));
  }
  for (const auto& alternative : alternatives) {
    if (flipped++ >= kMaxLowConfidenceConversions) break;
    const auto& segments = alternative.split.segments;
    std::vector<bool> flags(segments.size());
    for (size_t i = 0; i < segments.size(); ++i)
      flags[i] = i == alternative.english_segment ||
                 (segments[i].kind == core::MixedRomajiSegmentKind::English &&
                  segments[i].confidence == core::EnglishSegmentConfidence::High);
    if (auto candidate = convert(alternative.split, flags)) out.push_back(std::move(*candidate));
  }
  std::vector<ipc::CandidateField> unique;
  for (auto& field : out) {
    const bool seen = std::any_of(unique.begin(), unique.end(),
                                  [&](const auto& kept) { return kept.surface == field.surface; });
    if (!seen) unique.push_back(std::move(field));
  }
  if (unique.size() == 1 && !mixed_first && all_japanese) return {};
  return unique;
}

// M14 fallback scores are unbounded ranking values. A zero-baseline sigmoid
// preserves their ordering without pinning ordinary scores >1 to certainty;
// X-1 will calibrate against the runner-up candidate.
double LiveConfidence(double score) {
  const double e = std::exp(-std::abs(score));
  return score >= 0.0 ? 1.0 / (1.0 + e) : e / (1.0 + e);
}

// M63-B (sections 8.3 and 10): the live conversion of a split with a sure E
// segment, or nullopt to convert the TIP's kana as with M63 off: no sure
// segment, or the generation lost an English surface (the one conversion more
// that section 10 allows).
std::optional<ipc::QueryLiveConversionResponse> MixedLiveConversion(
    InferenceEngine& engine, const ipc::QueryLiveConversionRequest& request,
    const std::optional<core::MixedRomajiSplit>& split, const std::atomic<bool>* cancel,
    const InferenceTelemetry* telemetry) {
  if (!IsInlineSplit(split)) return std::nullopt;
  std::vector<bool> english(split->segments.size());
  for (size_t i = 0; i < split->segments.size(); ++i)
    english[i] = split->segments[i].kind == core::MixedRomajiSegmentKind::English &&
                 split->segments[i].confidence == core::EnglishSegmentConfidence::High;
  if (std::find(english.begin(), english.end(), true) == english.end()) return std::nullopt;
  const auto mixed = BuildMixedReading(*split, request.raw_romaji, english);
  MixedConversionOptions options;
  options.english_ranges = mixed.english_ranges;
  options.typo_correction = false;
  const auto candidate = engine.QueryLiveConversion(mixed.reading, request.context, NowSec(),
                                                    cancel, telemetry, &options);
  if (!candidate) return std::nullopt;
  ipc::QueryLiveConversionResponse response;
  if (std::isfinite(candidate->score)) response.confidence = LiveConfidence(candidate->score);
  auto segments = SegmentsOf(candidate->surface, mixed, request.raw_romaji, response.confidence);
  if (!segments) return std::nullopt;
  response.surface = candidate->surface;
  response.segments = std::move(*segments);
  return response;
}

// Section 9.2: the E segment of a candidate or a live conversion whose surface
// is the English word. The surface is printable ASCII, so its UTF-16 length is
// its byte length.
ipc::LiveSegment EnglishLiveSegment(const core::MixedRomajiSegment& english,
                                    const std::string& raw_romaji, double score) {
  ipc::LiveSegment segment;
  segment.end_char = static_cast<uint32_t>(english.surface.size());
  segment.score = score;
  segment.surface = english.surface;
  segment.reading = raw_romaji;
  segment.tag = static_cast<uint8_t>(core::CandidateTag::English);
  return segment;
}

// Section 8.2: a sure word becomes the first candidate ahead of the Japanese
// conversion; otherwise it is the second, right after the first Japanese one.
// The M60 form with the same surface gives up its place; a Japanese candidate
// with that surface stays.
void PlaceMixedEnglishCandidate(std::vector<core::Candidate>& candidates,
                                const core::MixedRomajiSegment& english,
                                const std::string& raw_romaji, bool first) {
  candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                  [&](const core::Candidate& c) {
                                    return c.tag == core::CandidateTag::English &&
                                           c.surface == english.surface;
                                  }),
                   candidates.end());
  core::Candidate candidate;
  candidate.surface = english.surface;
  candidate.reading = raw_romaji;  // M60 section 6.3: the raw romaji.
  candidate.source = core::CandidateSource::Heuristic;
  candidate.tag = core::CandidateTag::English;
  const size_t at = first ? 0 : std::min<size_t>(1, candidates.size());
  // Tie with a neighbour so a score-ordered consumer keeps the slot.
  if (!candidates.empty()) candidate.score = candidates[at == 0 ? 0 : at - 1].score;
  candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(at), std::move(candidate));
}

}  // namespace

Dispatcher::Dispatcher(InferenceEngine* engine, RequestScheduler* scheduler,
                       learning::UserDictionary* user_dict, DispatcherConfig config,
                       SettingsStore* settings_store, learning::AutoWordStore* auto_word_store,
                       logging::RuntimeLogger* runtime_logger)
    : engine_(engine),
      scheduler_(scheduler),
      user_dict_(user_dict),
      settings_store_(settings_store),
      auto_word_store_(auto_word_store),
      runtime_logger_(runtime_logger),
      config_(std::move(config)) {
  if (!config_.update_config_mutex) {
    config_.update_config_mutex = std::make_shared<std::mutex>();
  }
}

Dispatcher::~Dispatcher() { SetClientId({}); }

std::optional<ipc::Envelope> Dispatcher::Dispatch(const ipc::Envelope& req) {
  if (req.type != ipc::MessageType::Handshake && RequiresAuthenticatedSession()) {
    return HandleUnauthenticated(req);
  }
  switch (req.type) {
    case ipc::MessageType::Handshake:
      return HandleHandshake(req);
    case ipc::MessageType::Ping:
      return HandlePing(req);
    case ipc::MessageType::Health:
      return HandleHealth(req);
    case ipc::MessageType::QueryDiagnostics:
      return HandleQueryDiagnostics(req);
    case ipc::MessageType::LoadModel:
      return HandleLoadModel(req);
    case ipc::MessageType::QueryCandidates:
      return HandleQueryCandidates(req);
    case ipc::MessageType::QueryLiveConversion:
      return HandleQueryLiveConversion(req);
    case ipc::MessageType::QueryPredictions:
      return HandleQueryPredictions(req);
    case ipc::MessageType::QueryBatchConversion:
      return HandleQueryBatchConversion(req);
    case ipc::MessageType::ReverseConvert:
      return HandleReverseConvert(req);
    case ipc::MessageType::ListModels:
      return HandleListModels(req);
    case ipc::MessageType::BenchmarkModel:
      return HandleBenchmarkModel(req);
    case ipc::MessageType::FetchCatalogModel:
      return HandleFetchCatalogModel(req);
    case ipc::MessageType::QueryCatalogFetch:
      return HandleQueryCatalogFetch(req);
    case ipc::MessageType::Cancel:
      HandleCancel(req);
      return std::nullopt;
    case ipc::MessageType::CommitObservation:
      return HandleCommitObservation(req);
    case ipc::MessageType::CommitCorrection:
      return HandleCommitCorrection(req);
    case ipc::MessageType::CommitSegmentsObservation: {
      auto privacy =
          settings_store_ ? std::optional{settings_store_->LockPrivacyPolicy()} : std::nullopt;
      ipc::CommitObservationResponse response;
      if (auto parsed = ipc::ParseCommitSegmentsObservationRequest(req.payload_json);
          parsed && LearningAllowed(
                        parsed->secure, parsed->learning_allowed,
                        privacy && (privacy->policy.secure || !privacy->policy.learning_allowed))) {
        engine_->CommitSegmentsObservation(*parsed, NowSec());
        response.ok = true;
      }
      return MakeResponse(req, ipc::BuildCommitObservationResponse(response));
    }
    case ipc::MessageType::AddUserWord:
      return HandleAddUserWord(req);
    case ipc::MessageType::RemoveUserWord:
      return HandleRemoveUserWord(req);
    case ipc::MessageType::UpdateConfig:
      return HandleUpdateConfig(req);
    case ipc::MessageType::ObserveTypo:
      // Fire-and-forget (spec section 7): no reply is sent at all.
      HandleObserveTypo(req);
      return std::nullopt;
    case ipc::MessageType::ListNewWordCandidates:
      return HandleListNewWordCandidates(req);
    case ipc::MessageType::ListLearningEntries:
      return HandleListLearningEntries(req);
    case ipc::MessageType::ForgetLearningEntry:
      return HandleForgetLearningEntry(req);
    case ipc::MessageType::ExportLearningData:
      return HandleExportLearningData(req);
    case ipc::MessageType::ImportLearningData:
      return HandleImportLearningData(req);
    case ipc::MessageType::ResetLearningStore:
      return HandleResetLearningStore(req);
    case ipc::MessageType::QueryPersona:
      return HandleQueryPersona(req);
    case ipc::MessageType::DetectAnomalies:
      return HandleDetectAnomalies(req);
    case ipc::MessageType::ResolveNewWord:
      return HandleResolveNewWord(req);
    default:
      // Preserve the request type in the response envelope so future clients can
      // correlate the protocol error without waiting for a timeout.
      return MakeResponse(req, R"({"ok":false,"error":"unsupported_message_type"})");
  }
}

std::optional<ipc::Envelope> Dispatcher::HandleUnauthenticated(const ipc::Envelope& req) {
  // Return a type-appropriate error response so blocking clients do not hang
  // waiting for a reply that would never come if we returned nullopt.
  switch (req.type) {
    case ipc::MessageType::AddUserWord: {
      ipc::AddUserWordResponse r;
      r.ok = false;
      r.error = std::string(ipc::kUserDictErrorNotAuthenticated);
      return MakeResponse(req, ipc::BuildAddUserWordResponse(r));
    }
    case ipc::MessageType::RemoveUserWord: {
      ipc::RemoveUserWordResponse r;
      r.ok = false;
      r.error = std::string(ipc::kUserDictErrorNotAuthenticated);
      return MakeResponse(req, ipc::BuildRemoveUserWordResponse(r));
    }
    case ipc::MessageType::UpdateConfig: {
      ipc::UpdateConfigResponse r;
      r.ok = false;
      r.error = "not authenticated";
      return MakeResponse(req, ipc::BuildUpdateConfigResponse(r));
    }
    case ipc::MessageType::CommitSegmentsObservation:
    case ipc::MessageType::CommitObservation:
    case ipc::MessageType::CommitCorrection: {
      ipc::CommitObservationResponse r;
      r.ok = false;
      return MakeResponse(req, ipc::BuildCommitObservationResponse(r));
    }
    case ipc::MessageType::ObserveTypo:
      // Fire-and-forget either way: dropping it silently is the same shape an
      // authenticated session gives, and nothing is recorded.
      return std::nullopt;
    case ipc::MessageType::ListNewWordCandidates: {
      // No items rather than the store's contents: the approval list is built
      // from what the user typed.
      ipc::ListNewWordCandidatesResponse r;
      r.ok = false;
      r.error = std::string(ipc::kNewWordErrorNotAuthenticated);
      return MakeResponse(req, ipc::BuildListNewWordCandidatesResponse(r));
    }
    case ipc::MessageType::ResolveNewWord: {
      ipc::ResolveNewWordResponse r;
      r.ok = false;
      r.error = std::string(ipc::kNewWordErrorNotAuthenticated);
      return MakeResponse(req, ipc::BuildResolveNewWordResponse(r));
    }
    case ipc::MessageType::LoadModel: {
      ipc::LoadModelResponse r;
      r.ok = false;
      r.error = "not authenticated";
      return MakeResponse(req, ipc::BuildLoadModelResponse(r));
    }
    case ipc::MessageType::QueryCandidates: {
      ipc::QueryCandidatesResponse r;
      r.partial = false;
      return MakeResponse(req, ipc::BuildQueryCandidatesResponse(r));
    }
    case ipc::MessageType::QueryLiveConversion:
      return MakeResponse(req, ipc::BuildQueryLiveConversionResponse({}));
    case ipc::MessageType::QueryPredictions: {
      ipc::QueryPredictionsResponse r;
      r.ok = false;
      r.error = "not authenticated";
      return MakeResponse(req, ipc::BuildQueryPredictionsResponse(r));
    }
    case ipc::MessageType::QueryBatchConversion: {
      ipc::QueryBatchConversionResponse r;
      if (auto parsed = ipc::ParseQueryBatchConversionRequest(req.payload_json)) {
        r.full_surface = parsed->reading;
      }
      r.partial = false;
      r.canceled = false;
      return MakeResponse(req, ipc::BuildQueryBatchConversionResponse(r));
    }
    case ipc::MessageType::ReverseConvert:
      return MakeResponse(req, ipc::BuildReverseConvertResponse(ipc::ReverseConvertResponse{}));
    case ipc::MessageType::ListLearningEntries: {
      ipc::ListLearningEntriesResponse r;
      r.ok = false;
      r.error = std::string(ipc::kLearningDataErrorNotAuthenticated);
      return MakeResponse(req, ipc::BuildListLearningEntriesResponse(r));
    }
    case ipc::MessageType::ForgetLearningEntry: {
      ipc::ForgetLearningEntryResponse r;
      r.ok = false;
      r.error = std::string(ipc::kLearningDataErrorNotAuthenticated);
      return MakeResponse(req, ipc::BuildForgetLearningEntryResponse(r));
    }
    case ipc::MessageType::ExportLearningData: {
      ipc::ExportLearningDataResponse r;
      r.error = std::string(ipc::kLearningDataErrorNotAuthenticated);
      return MakeResponse(req, ipc::BuildExportLearningDataResponse(r));
    }
    case ipc::MessageType::ImportLearningData: {
      ipc::ImportLearningDataResponse r;
      r.error = std::string(ipc::kLearningDataErrorNotAuthenticated);
      return MakeResponse(req, ipc::BuildImportLearningDataResponse(r));
    }
    case ipc::MessageType::ResetLearningStore: {
      ipc::ResetLearningStoreResponse r;
      r.ok = false;
      r.error = std::string(ipc::kLearningDataErrorNotAuthenticated);
      return MakeResponse(req, ipc::BuildResetLearningStoreResponse(r));
    }
    case ipc::MessageType::QueryPersona: {
      ipc::QueryPersonaResponse r;
      r.ok = false;
      r.error = std::string(ipc::kLearningDataErrorNotAuthenticated);
      return MakeResponse(req, ipc::BuildQueryPersonaResponse(r));
    }
    case ipc::MessageType::DetectAnomalies: {
      ipc::DetectAnomaliesResponse r;
      r.ok = false;
      r.error = std::string(ipc::kLearningDataErrorNotAuthenticated);
      return MakeResponse(req, ipc::BuildDetectAnomaliesResponse(r));
    }
    case ipc::MessageType::ListModels: {
      ipc::ListModelsResponse r;
      r.ok = false;
      r.error = "not authenticated";
      return MakeResponse(req, ipc::BuildListModelsResponse(r));
    }
    case ipc::MessageType::BenchmarkModel: {
      ipc::BenchmarkModelResponse r;
      r.error = "not authenticated";
      return MakeResponse(req, ipc::BuildBenchmarkModelResponse(r));
    }
    case ipc::MessageType::FetchCatalogModel:
    case ipc::MessageType::QueryCatalogFetch: {
      ipc::CatalogFetchResponse r;
      r.ok = false;
      r.error = "not authenticated";
      return MakeResponse(req, ipc::BuildCatalogFetchResponse(r));
    }
    case ipc::MessageType::Ping: {
      ipc::PingPayload p;
      p.nonce = 0;
      p.t_ms = 0;
      return MakeResponse(req, ipc::BuildPing(p));
    }
    case ipc::MessageType::Health: {
      ipc::HealthPayload p;
      p.status = "error";
      p.backend = "";
      p.model_loaded = false;
      p.last_error = "not authenticated";
      return MakeResponse(req, ipc::BuildHealth(p));
    }
    case ipc::MessageType::QueryDiagnostics: {
      ipc::QueryDiagnosticsPayload p;
      p.engine = config_.runtime_tier;
      p.backend = "";
      p.fallback_state = "healthy";
      p.last_error = "not authenticated";
      return MakeResponse(req, ipc::BuildQueryDiagnostics(p));
    }
    default:
      // Cancel and unknown types are fire-and-forget; no response needed.
      return std::nullopt;
  }
}

std::optional<ipc::Envelope> Dispatcher::HandleHandshake(const ipc::Envelope& req) {
  client_supports_secure_flag_ = false;
  ipc::HandshakeResponse res;
  res.host_version = config_.host_version;
  res.protocol_version = config_.protocol_version;
  res.host_generation_id = config_.host_generation_id;
  // M48: app_profile = honors QueryCandidates.app; candidate_tag = fills
  // CandidateField.tag (docs/app-profile-spec.md sections 3.1 and 7).
  // mixed_english = M63 (mixed-english-auto-conversion-spec section 9.3).
  res.capabilities = {"oob_cancel",
                      "commit_segments",
                      "query_live_conversion",
                      "query_predictions",
                      "app_profile",
                      "candidate_tag",
                      "list_models",
                      "benchmark_model",
                      "english_candidates",
                      "learning_data_management",
                      "commit_correction",
                      "learning_reset",
                      "persona",
                      "detect_anomalies",
                      "mixed_english"};
  // DEV-1821 (model-management-spec section 4.4).
  if (config_.catalog_fetch) res.capabilities.push_back("fetch_catalog_model");
  if (auto parsed = ipc::ParseHandshakeRequest(req.payload_json)) {
    const bool version_ok = parsed->protocol_version == config_.protocol_version;
    const bool token_ok = config_.handshake_token.empty() ||
                          ConstantTimeEquals(parsed->handshake_token, config_.handshake_token);
    res.accepted = version_ok && token_ok;
    client_supports_secure_flag_ =
        res.accepted && std::find(parsed->capabilities.begin(), parsed->capabilities.end(),
                                  "secure_flag") != parsed->capabilities.end();
    SetClientId(res.accepted ? std::move(parsed->client_id) : std::string());
  } else {
    res.accepted = false;
    SetClientId({});
  }
  authenticated_ = res.accepted;
  res.model_loaded = engine_->model_loaded();
  if (settings_store_) {
    // The settings app writes settings.json before it sends UpdateConfig, so a
    // TIP that re-handshakes on the file change can arrive first and would
    // otherwise be answered with the values it already has (DEV-1143). Read
    // the newer file directly rather than reloading the store: taking
    // update_config_mutex here would park this reply behind the model reload
    // UpdateConfig performs under it, past the TIP's handshake timeout.
    const auto written = settings_store_->SettingsWrittenAfterLoad();
    const auto& settings = written ? *written : settings_store_->settings();
    res.batch_romaji_conversion = settings.batch_romaji_conversion;
    res.batch_romaji_preview_style = settings.batch_romaji_preview_style;
    res.batch_conversion_mode = settings.batch_conversion_mode;
    res.batch_auto_punctuation = settings.batch_auto_punctuation;
    res.number_rewriter = settings.number_rewriter;
    res.katakana_rewriter = settings.katakana_rewriter;
    res.symbol_rewriter = settings.symbol_rewriter;
    res.emoji_rewriter = settings.emoji_rewriter;
    res.emoji_trigger_search = settings.emoji_trigger_search;
    res.emoji_max_candidates = static_cast<uint32_t>(settings.emoji_max_candidates);
    res.emoji_trigger_min_query_length =
        static_cast<uint32_t>(settings.emoji_trigger_min_query_length);
    res.max_candidates = static_cast<uint32_t>(settings.max_candidates);
  }
  return MakeResponse(req, ipc::BuildHandshakeResponse(res));
}

bool Dispatcher::RequiresAuthenticatedSession() const {
  return !config_.handshake_token.empty() && !authenticated_;
}

bool Dispatcher::LearningAllowed(bool secure, bool learning_allowed,
                                 bool host_learning_blocked) const {
  return client_supports_secure_flag_ && !secure && learning_allowed && !host_learning_blocked;
}

void Dispatcher::SetClientId(std::string client_id) {
  if (client_id == client_id_) return;

  if (!client_id_.empty()) {
    scheduler_->UnregisterClient(client_id_);
  }
  client_id_ = std::move(client_id);
  if (!client_id_.empty()) {
    scheduler_->RegisterClient(client_id_);
  }
}

std::optional<ipc::Envelope> Dispatcher::HandlePing(const ipc::Envelope& req) {
  auto parsed = ipc::ParsePing(req.payload_json);
  ipc::PingPayload res;
  res.nonce = parsed ? parsed->nonce : 0;
  res.t_ms = NowMs();
  return MakeResponse(req, ipc::BuildPing(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleHealth(const ipc::Envelope& req) {
  ipc::HealthPayload p;
  const auto engine_health = engine_->health_snapshot();
  p.backend = BackendName(engine_health.backend);
  p.model_loaded = engine_health.model_loaded;
  p.last_error = engine_health.last_error;
  if (!p.last_error) {
    p.status = "ok";
  } else if (p.model_loaded || !engine_health.model_path.empty()) {
    p.status = "degraded";
  } else {
    p.status = "error";
  }
  return MakeResponse(req, ipc::BuildHealth(p));
}

std::optional<ipc::Envelope> Dispatcher::HandleQueryDiagnostics(const ipc::Envelope& req) {
  const auto engine_health = engine_->health_snapshot();
  ipc::QueryDiagnosticsPayload p;
  const bool effective_model_loaded = engine_health.model_loaded && config_.runtime_tier != "mock";
  p.model_loaded = effective_model_loaded;
  if (effective_model_loaded && !engine_health.model_path.empty()) {
    p.loaded_model_path = engine_health.model_path;
  }
  p.engine = config_.runtime_tier;
  p.backend = BackendName(engine_health.backend);
  p.rss_mb = CurrentRssMb();
  p.learning_entries = static_cast<uint64_t>(engine_health.learning_entries);
  p.user_dict_entries = static_cast<uint64_t>(engine_health.user_dict_entries);
  p.last_error = engine_health.last_error;
  if (effective_model_loaded && engine_health.prompt_format) {
    p.prompt_format = std::string(ZenzaiPromptFormatName(*engine_health.prompt_format));
  }

  const bool model_enabled = !settings_store_ || settings_store_->settings().model.enabled;
  // Section 12.6 / D-009: SafeMode outranks everything, model.enabled=false
  // included, because it is the one state the user has to act on.
  if (engine_health.health_state == HealthState::SafeMode) {
    p.fallback_state = "safe_mode";
  } else if (!model_enabled) {
    p.fallback_state = "healthy";
  } else if (engine_health.health_state == HealthState::DegradedModel ||
             engine_health.health_state == HealthState::RecoveringModel) {
    p.fallback_state = "degraded_model";
  } else {
    p.fallback_state = "healthy";
  }
  if (config_.neologd_layer) p.neologd_layer = config_.neologd_layer->Snapshot();
  p.backend_selection = config_.backend_selection;
  return MakeResponse(req, ipc::BuildQueryDiagnostics(p));
}

bool Dispatcher::SafeModeEnabled() const {
  // The engine's state, not the settings: UpdateConfig holds its lock through
  // a model reload, and the engine is kept in step with the flag at startup
  // and on every UpdateConfig.
  return engine_->health_state() == HealthState::SafeMode;
}

std::optional<ipc::Envelope> Dispatcher::HandleLoadModel(const ipc::Envelope& req) {
  ipc::LoadModelResponse res;
  auto parsed = ipc::ParseLoadModelRequest(req.payload_json);
  if (!parsed) {
    res.ok = false;
    res.error = "invalid LoadModel payload";
  } else if (SafeModeEnabled()) {
    // Section 8.5.3: no model runs in SafeMode, whoever asks.
    res.ok = false;
    res.error = "safe_mode";
  } else {
    auto backend = ParseBackend(parsed->backend);
    if (!backend) {
      res.ok = false;
      res.error = "unsupported backend: " + parsed->backend;
    } else {
      ModelLoadOptions options;
      options.path = parsed->path;
      options.backend = *backend;
      options.n_gpu_layers = parsed->n_gpu_layers;
      options.resolve_prompt_format =
          CatalogPromptFormatResolver(config_.catalog_matcher, options.path);
      const auto load_result = engine_->LoadModelWithResult(options);
      res.ok = load_result.ok;
      res.error = load_result.error;
      if (!res.ok && !res.error) res.error = "model load failed";
    }
  }
  return MakeResponse(req, ipc::BuildLoadModelResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleQueryCandidates(const ipc::Envelope& req) {
  // M63-B (mixed-english-auto-conversion-spec section 10): the deadline of the
  // mixed conversions counts from the receipt.
  const auto received = std::chrono::steady_clock::now();
  auto parsed = ipc::ParseQueryCandidatesRequest(req.payload_json);
  InferenceTrace trace(req.request_id, client_id_, req.trace_id, core::EtwBackend::Unknown,
                       parsed ? parsed->reading.size() : 0, runtime_logger_);
  if (!parsed) {
    trace.ProtocolError();
    ipc::QueryCandidatesResponse res;
    res.partial = false;
    return MakeResponse(req, ipc::BuildQueryCandidatesResponse(res));
  }
  const auto queue_start =
      runtime_logger_ && runtime_logger_->enabled() && ipc::IsValidTraceId(req.trace_id)
          ? std::optional{std::chrono::steady_clock::now()}
          : std::nullopt;
  auto cancel = scheduler_->TrackCancellation(client_id_, req.request_id);
  if (!cancel) {
    LogHostQueueWait(runtime_logger_, req, queue_start, "error");
    // Capacity rejection is a failure, distinct from a tracked cancellation.
    return MakeResponse(req, ipc::BuildQueryCandidatesResponse(ipc::QueryCandidatesResponse{}));
  }
  scheduler_->MarkLatest(client_id_, req.request_id);
  LogHostQueueWait(runtime_logger_, req, queue_start, "ok");
  trace.WatchCancellation(cancel);
  RequestCompletionGuard completion(scheduler_, client_id_, req.request_id);

  const auto engine_config = engine_->config();
  const auto rewriters = engine_config.rewriters;
  std::vector<core::Candidate> candidates;
  std::string corrected_reading;
  std::optional<core::MixedRomajiSegment> mixed_first;
  std::vector<ipc::CandidateField> mixed_front;
  bool mixed_after_first = false;  // Section 8.3 item 3 behind the first of M63 off.
  if (!parsed->emoji_trigger.empty()) {
    if (!parsed->reading.empty()) {
      trace.ProtocolError();
      static std::atomic_flag warned = ATOMIC_FLAG_INIT;
      if (!warned.test_and_set()) {
        DispatcherLogger(runtime_logger_)
            .Log(logging::RuntimeLogLevel::Warn, "invalid_mixed_emoji_query",
                 {{"request_id", req.request_id},
                  {"error_code", logging::RuntimeLogSafeText("protocol")}});
        core::EtwLogger::LogError(core::EtwModule::Host, core::EtwErrorCode::Protocol,
                                  kGenericFailureHresult);
      }
      ipc::QueryCandidatesResponse invalid;
      invalid.ok = false;
      invalid.error = "reading and emoji_trigger are mutually exclusive";
      completion.Complete();
      return MakeResponse(req, ipc::BuildQueryCandidatesResponse(invalid));
    } else if (!parsed->live) {
      candidates =
          engine_->QueryRewriters(rewriters, {}, parsed->emoji_trigger, {}, parsed->max_candidates);
    }
  } else {
    const uint32_t reserve =
        parsed->live ? 0
                     : (rewriters.symbol_enabled ? 4u : 0u) + (rewriters.emoji_enabled ? 4u : 0u);
    const auto ordinary_limit = parsed->max_candidates;
    const size_t merged_limit = ordinary_limit == 0 ? 0 : size_t{ordinary_limit} + reserve;
    const auto tag_boosts = ResolveTagBoosts(engine_config, parsed->app);
    // M63 (mixed-english-auto-conversion-spec sections 8 and 9.4): the candidate
    // window only, and never for a secure request (DEV-1689).
    const bool mixed_english = !parsed->live && !parsed->secure && parsed->english_candidates &&
                               !parsed->raw_romaji.empty() && parsed->mixed_english;
    std::vector<core::MixedRomajiAlternative> alternatives;
    const auto split =
        mixed_english
            ? SplitComposition(*engine_, parsed->raw_romaji,
                               config_.mixed_english_inline_high_confidence, &alternatives)
            : std::nullopt;
    // Section 12: a composition split into J and E segments is converted as
    // typed, so no corrected reading comes back with its segments.
    MixedConversionOptions as_typed;
    as_typed.typo_correction = false;
    // DEV-1734: the candidate window also weighs the other reading of an
    // ambiguous "nn" + vowel. Per-key queries, a secure request (its TIP drops
    // corrected_reading) and an M63 split are converted as typed.
    auto queried =
        !parsed->live && !parsed->secure && !parsed->alt_readings.empty() && !IsInlineSplit(split)
            ? engine_->QueryAmbiguousCandidatesEx(parsed->reading, parsed->alt_readings,
                                                  parsed->left_context, NowSec(), cancel.get(),
                                                  ordinary_limit, trace.context(), &tag_boosts)
            : engine_->QueryCandidatesEx(parsed->reading, parsed->left_context, NowSec(),
                                         cancel.get(), ordinary_limit, parsed->live,
                                         trace.context(), &tag_boosts,
                                         IsInlineSplit(split) ? &as_typed : nullptr);
    const auto all_japanese = IsInlineSplit(split) && !queried.candidates.empty()
                                  ? std::optional{queried.candidates.front()}
                                  : std::nullopt;
    candidates = std::move(queried.candidates);
    corrected_reading = std::move(queried.corrected_reading);
    // Under auto_replace the conversion ran on the corrected reading, so the
    // rewriters have to key off the same reading or they would look up the
    // typo the user is no longer being shown.
    const std::string& rewriter_reading =
        corrected_reading.empty() ? parsed->reading : corrected_reading;
    if (!parsed->live && (rewriters.symbol_enabled || rewriters.emoji_enabled))
      candidates = engine_->QueryRewriters(rewriters, rewriter_reading, {}, std::move(candidates),
                                           merged_limit);
    // M60: the candidate window only (section 4.1); live conversion takes the
    // first candidate, which English never claims anyway. The romaji is the
    // user's input, so a secure request never uses it (DEV-1689).
    if (!parsed->live && !parsed->secure && parsed->english_candidates &&
        !parsed->raw_romaji.empty()) {
      auto english = engine_->QueryEnglishCandidates(parsed->raw_romaji, NowSec());
      PlaceEnglishCandidates(candidates, std::move(english.candidates), english.intent,
                             engine_config.english.promote_threshold);
      // M63-A (mixed-english-auto-conversion-spec sections 8.2 and 9.4): the
      // whole composition as one English word, on top of M60.
      if (parsed->mixed_english) {
        if (auto word = WholeCompositionEnglish(split)) {
          const bool sure = word->confidence == core::EnglishSegmentConfidence::High;
          PlaceMixedEnglishCandidate(candidates, *word, parsed->raw_romaji, sure);
          // corrected_reading stays: the Japanese candidates were converted
          // from it (M35), and the English one is learned under its own
          // reading, the raw romaji (M60 section 6.4).
          if (sure) mixed_first = std::move(word);
        } else if (IsInlineSplit(split)) {
          // M63-B section 8.3, on top of M60.
          mixed_front = MixedCandidates(*engine_, *parsed, *split, alternatives, all_japanese,
                                        received + config_.mixed_english_conversion_deadline,
                                        cancel.get(), trace.context(), &tag_boosts);
        } else if (split && !alternatives.empty() && corrected_reading.empty()) {
          // Section 8.3 item 3 for a split of one J segment: the list of M63
          // off, typo correction and the DEV-1734 reading included, with the
          // readable words turned English right after its first candidate. A
          // corrected reading is not the typed one the words come from.
          mixed_front = MixedCandidates(*engine_, *parsed, *split, alternatives, std::nullopt,
                                        received + config_.mixed_english_conversion_deadline,
                                        cancel.get(), trace.context(), &tag_boosts);
          mixed_after_first = true;
        }
      }
    }
  }

  const bool canceled = cancel->load(std::memory_order_acquire);
  completion.Complete();
  if (canceled) {
    trace.Finish(core::EtwResult::Cancelled);
    return std::nullopt;  // don't reply to canceled requests
  }

  ipc::QueryCandidatesResponse res;
  for (auto& c : candidates) res.candidates.push_back(ToField(c));
  // Section 9.2: only the English first candidate of section 8.2 carries
  // segments.
  if (mixed_first && !res.candidates.empty())
    res.candidates.front().segments = {
        EnglishLiveSegment(*mixed_first, parsed->raw_romaji, res.candidates.front().score)};
  // Section 8.3 for a split of one J segment: the first candidate of M63 off
  // stays as it is, item 3 follows it, and the rest follow without their
  // surfaces. Between the two by score, so a score-ordered consumer keeps it.
  if (!mixed_front.empty() && mixed_after_first && !res.candidates.empty()) {
    const auto& first = res.candidates.front();
    std::erase_if(mixed_front, [&](const auto& front) { return front.surface == first.surface; });
    res.candidates.erase(std::remove_if(res.candidates.begin() + 1, res.candidates.end(),
                                        [&](const ipc::CandidateField& c) {
                                          return std::any_of(mixed_front.begin(), mixed_front.end(),
                                                             [&](const auto& front) {
                                                               return front.surface == c.surface;
                                                             });
                                        }),
                         res.candidates.end());
    const double top = res.candidates.front().score;
    for (auto& front : mixed_front) {
      if (res.candidates.size() > 1) front.score = (std::max)(front.score, res.candidates[1].score);
      front.score = (std::min)(front.score, top);
    }
    res.candidates.insert(res.candidates.begin() + 1, std::make_move_iterator(mixed_front.begin()),
                          std::make_move_iterator(mixed_front.end()));
    mixed_front.clear();
  }
  // Section 8.3: items 1 to 3 lead, and the rest follow without their surfaces.
  if (!mixed_front.empty() && !mixed_after_first) {
    std::erase_if(res.candidates, [&](const ipc::CandidateField& c) {
      return std::any_of(mixed_front.begin(), mixed_front.end(),
                         [&](const auto& front) { return front.surface == c.surface; });
    });
    // Never below the first of the rest, so items 1 to 3 stay ahead of it for
    // a score-ordered consumer; their own order is the position.
    if (!res.candidates.empty())
      for (auto& front : mixed_front)
        front.score = (std::max)(front.score, res.candidates.front().score);
    res.candidates.insert(res.candidates.begin(), std::make_move_iterator(mixed_front.begin()),
                          std::make_move_iterator(mixed_front.end()));
  }
  // The pre-M14 carrier of live conversion; current TIPs use QueryLiveConversion.
  if (parsed->live && !res.candidates.empty()) {
    if (auto inserted = InsertLivePunctuation(
            engine_config, parsed->auto_punctuation, parsed->punctuation_style,
            res.candidates.front().surface, res.candidates.front().reading)) {
      res.candidates.front().surface = std::move(inserted->surface);
      res.segments = std::move(inserted->segments);
    }
  }
  if (!parsed->emoji_trigger.empty() && parsed->max_candidates > 0 &&
      res.candidates.size() > parsed->max_candidates) {
    res.candidates.resize(parsed->max_candidates);
  }
  res.partial = false;
  res.corrected_reading = std::move(corrected_reading);
  trace.Finish(core::EtwResult::Success, res.candidates.size());
  return MakeResponse(req, ipc::BuildQueryCandidatesResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleQueryLiveConversion(const ipc::Envelope& req) {
  const auto parsed = ipc::ParseQueryLiveConversionRequest(req.payload_json);
  InferenceTrace trace(req.request_id, client_id_, req.trace_id, core::EtwBackend::Kana,
                       parsed ? parsed->kana.size() : 0, runtime_logger_);
  ipc::QueryLiveConversionResponse response;
  if (!parsed || parsed->kana.empty()) {
    trace.ProtocolError();
    return MakeResponse(req, ipc::BuildQueryLiveConversionResponse(response));
  }

  const auto queue_start =
      runtime_logger_ && runtime_logger_->enabled() && ipc::IsValidTraceId(req.trace_id)
          ? std::optional{std::chrono::steady_clock::now()}
          : std::nullopt;
  auto cancel = scheduler_->TrackCancellation(client_id_, req.request_id);
  if (!cancel) {
    LogHostQueueWait(runtime_logger_, req, queue_start, "error");
    return MakeResponse(req, ipc::BuildQueryLiveConversionResponse(response));
  }
  scheduler_->MarkLatest(client_id_, req.request_id);
  LogHostQueueWait(runtime_logger_, req, queue_start, "ok");
  trace.WatchCancellation(cancel);
  RequestCompletionGuard completion(scheduler_, client_id_, req.request_id);

  const auto punctuation_config =
      parsed->auto_punctuation ? std::optional{engine_->config()} : std::nullopt;
  // M63-A (mixed-english-auto-conversion-spec sections 8.2, 9.4 and 10): a
  // sure English word is the live conversion as is, without Zenzai.
  if (!parsed->secure && parsed->mixed_english && !parsed->raw_romaji.empty()) {
    const auto split = SplitComposition(*engine_, parsed->raw_romaji,
                                        config_.mixed_english_inline_high_confidence);
    if (auto word = WholeCompositionEnglish(split);
        word && word->confidence == core::EnglishSegmentConfidence::High) {
      const bool canceled = cancel->load(std::memory_order_acquire);
      completion.Complete();
      if (canceled) {
        trace.Finish(core::EtwResult::Cancelled);
        return std::nullopt;
      }
      response.surface = word->surface;
      response.confidence = 1.0;
      response.segments = {EnglishLiveSegment(*word, parsed->raw_romaji, 1.0)};
      if (punctuation_config) {
        // M59 as with M63 off; the word's segments keep the English tag.
        if (auto inserted =
                InsertLivePunctuation(*punctuation_config, true, parsed->punctuation_style,
                                      word->surface, parsed->raw_romaji)) {
          response.surface = std::move(inserted->surface);
          response.segments = std::move(inserted->segments);
          for (auto& segment : response.segments)
            if (!segment.auto_punctuation)
              segment.tag = static_cast<uint8_t>(core::CandidateTag::English);
        }
      }
      trace.Finish(core::EtwResult::Success, 1);
      return MakeResponse(req, ipc::BuildQueryLiveConversionResponse(response));
    }
    // M63-B (sections 8.3 and 10): one Zenzai conversion of the reading with
    // the sure E segments in English, as typed (section 12).
    if (auto mixed = MixedLiveConversion(*engine_, *parsed, split, cancel.get(), trace.context())) {
      const bool canceled = cancel->load(std::memory_order_acquire);
      completion.Complete();
      if (canceled) {
        trace.Finish(core::EtwResult::Cancelled);
        return std::nullopt;
      }
      response = std::move(*mixed);
      if (punctuation_config) {
        // M59 as with M63 off, judged on the whole surface; the mark joins the
        // segments, which keep their tags.
        if (auto inserted =
                InsertLivePunctuation(*punctuation_config, true, parsed->punctuation_style,
                                      response.surface, parsed->kana)) {
          for (auto& segment : inserted->segments) {
            if (!segment.auto_punctuation || segment.start_char < Utf16Length(response.surface))
              continue;
            response.segments.push_back(std::move(segment));
          }
          response.surface = std::move(inserted->surface);
        }
      }
      trace.Finish(core::EtwResult::Success, 1);
      return MakeResponse(req, ipc::BuildQueryLiveConversionResponse(response));
    }
    if (cancel->load(std::memory_order_acquire)) {
      completion.Complete();
      trace.Finish(core::EtwResult::Cancelled);
      return std::nullopt;
    }
  }
  const auto candidate = engine_->QueryLiveConversion(parsed->kana, parsed->context, NowSec(),
                                                      cancel.get(), trace.context());
  const bool canceled = cancel->load(std::memory_order_acquire);
  completion.Complete();
  if (canceled) {
    trace.Finish(core::EtwResult::Cancelled);
    return std::nullopt;
  }
  if (candidate) {
    response.surface = candidate->surface;
    // The reading the candidate was converted from: a typo auto-replace has
    // already corrected it, and the segments feed learning (section 5.3).
    const auto& reading = candidate->reading.empty() ? parsed->kana : candidate->reading;
    if (punctuation_config) {
      if (auto inserted = InsertLivePunctuation(
              *punctuation_config, true, parsed->punctuation_style, candidate->surface, reading)) {
        response.surface = std::move(inserted->surface);
        response.segments = std::move(inserted->segments);
      }
    }
    if (std::isfinite(candidate->score)) response.confidence = LiveConfidence(candidate->score);
  }
  trace.Finish(core::EtwResult::Success, candidate ? 1 : 0);
  return MakeResponse(req, ipc::BuildQueryLiveConversionResponse(response));
}

std::optional<ipc::Envelope> Dispatcher::HandleQueryPredictions(const ipc::Envelope& req) {
  const auto parsed = ipc::ParseQueryPredictionsRequest(req.payload_json);
  InferenceTrace trace(req.request_id, client_id_, req.trace_id, core::EtwBackend::Kana,
                       parsed ? parsed->kana.size() : 0, runtime_logger_);
  ipc::QueryPredictionsResponse response;
  if (!parsed || parsed->kana.empty()) {
    trace.ProtocolError();
    response.ok = false;
    response.error = "invalid_request";
    return MakeResponse(req, ipc::BuildQueryPredictionsResponse(response));
  }
  if (parsed->mode != "word") {
    trace.ProtocolError();
    response.ok = false;
    response.error = "unsupported_prediction_mode";
    return MakeResponse(req, ipc::BuildQueryPredictionsResponse(response));
  }

  const auto queue_start =
      runtime_logger_ && runtime_logger_->enabled() && ipc::IsValidTraceId(req.trace_id)
          ? std::optional{std::chrono::steady_clock::now()}
          : std::nullopt;
  auto cancel = scheduler_->TrackCancellation(client_id_, req.request_id);
  if (!cancel) {
    LogHostQueueWait(runtime_logger_, req, queue_start, "error");
    response.ok = false;
    response.error = "too_many_pending_requests";
    return MakeResponse(req, ipc::BuildQueryPredictionsResponse(response));
  }
  scheduler_->MarkLatest(client_id_, req.request_id);
  LogHostQueueWait(runtime_logger_, req, queue_start, "ok");
  trace.WatchCancellation(cancel);
  RequestCompletionGuard completion(scheduler_, client_id_, req.request_id);

  auto candidates = engine_->QueryPredictions(parsed->kana, parsed->left_side_context, NowSec(),
                                              cancel.get(), trace.context());
  const bool canceled = cancel->load(std::memory_order_acquire);
  const bool stale = !scheduler_->IsLatest(client_id_, req.request_id);
  completion.Complete();
  if (canceled || stale) {
    trace.Finish(core::EtwResult::Cancelled);
    return std::nullopt;
  }

  for (const auto& candidate : candidates) response.predictions.push_back(ToField(candidate));
  trace.Finish(core::EtwResult::Success, response.predictions.size());
  return MakeResponse(req, ipc::BuildQueryPredictionsResponse(response));
}

void Dispatcher::HandleObserveTypo(const ipc::Envelope& req) {
  auto parsed = ipc::ParseObserveTypoRequest(req.payload_json);
  if (!parsed) return;
  auto privacy =
      settings_store_ ? std::optional{settings_store_->LockPrivacyPolicy()} : std::nullopt;
  if (!LearningAllowed(parsed->secure, parsed->learning_allowed,
                       privacy && (privacy->policy.secure || !privacy->policy.learning_allowed)))
    return;
  // The engine applies the accept filters and the "off" gate; a rejected pair
  // is simply not recorded, and there is no reply to carry that outcome.
  engine_->ObserveTypo(parsed->wrong_reading, parsed->correct_reading, NowSec());
}

std::optional<ipc::Envelope> Dispatcher::HandleListNewWordCandidates(const ipc::Envelope& req) {
  ipc::ListNewWordCandidatesResponse res;
  const auto fail = [&](std::string_view error) {
    res.ok = false;
    res.error = std::string(error);
    return MakeResponse(req, ipc::BuildListNewWordCandidatesResponse(res));
  };
  auto parsed = ipc::ParseListNewWordCandidatesRequest(req.payload_json);
  learning::AutoWordState state = learning::AutoWordState::Pending;
  // The parser already restricts state_filter to the three names, so a failed
  // ParseAutoWordState here would be a codec/store mismatch, not user input.
  if (!parsed || !learning::ParseAutoWordState(parsed->state_filter, state)) {
    return fail(ipc::kNewWordErrorInvalidRequest);
  }
  if (!auto_word_store_) return fail(ipc::kNewWordErrorStoreUnavailable);

  auto words = auto_word_store_->ListByState(state);
  // Most recently seen first, so a page of max_items shows the words the user
  // has been typing rather than an arbitrary slice of the map order.
  std::sort(words.begin(), words.end(),
            [](const learning::AutoWord& a, const learning::AutoWord& b) {
              if (a.last_seen_epoch != b.last_seen_epoch)
                return a.last_seen_epoch > b.last_seen_epoch;
              if (a.surface != b.surface) return a.surface < b.surface;
              return a.reading < b.reading;
            });
  if (words.size() > parsed->max_items) words.resize(parsed->max_items);

  res.items.reserve(words.size());
  for (const auto& word : words) {
    ipc::NewWordField field;
    field.surface = word.surface;
    field.reading = word.reading;
    field.source = std::string(learning::AutoWordSourceName(word.source));
    field.state = std::string(learning::AutoWordStateName(word.state));
    field.count = word.count;
    field.last_seen_epoch = word.last_seen_epoch;
    res.items.push_back(std::move(field));
  }
  return MakeResponse(req, ipc::BuildListNewWordCandidatesResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleResolveNewWord(const ipc::Envelope& req) {
  const auto received = std::chrono::steady_clock::now();
  ipc::ResolveNewWordResponse res;
  const auto reply = [&]() { return MakeResponse(req, ipc::BuildResolveNewWordResponse(res)); };
  const auto fail = [&](std::string_view error) {
    res.ok = false;
    res.error = std::string(error);
    return reply();
  };
  auto parsed = ipc::ParseResolveNewWordRequest(req.payload_json);
  if (!parsed) return fail(ipc::kNewWordErrorInvalidRequest);
  if (!auto_word_store_) return fail(ipc::kNewWordErrorStoreUnavailable);

  const auto target = parsed->action == "confirm" ? learning::AutoWordState::Confirmed
                                                  : learning::AutoWordState::Rejected;
  // DEV-1572: the store takes no file lock; its in-process wait and the save
  // are logged together as save_ms.
  const auto save_start = std::chrono::steady_clock::now();
  // Read before the save that may confirm the word: an AutoWord reset after
  // it cancels the English upsert below (DEV-1600).
  const auto english_epoch = engine_->EnglishRegistrationEpoch();
  const auto outcome = auto_word_store_->SetStateAndSave(parsed->surface, parsed->reading, target);
  UserDictionaryEditReport report;
  report.save_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - save_start)
          .count();
  report.total_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - received)
          .count();
  report.result =
      outcome == learning::AutoWordSaveOutcome::NotFound     ? UserDictionaryEditResult::NotFound
      : outcome == learning::AutoWordSaveOutcome::SaveFailed ? UserDictionaryEditResult::SaveFailed
                                                             : UserDictionaryEditResult::Ok;
  LogPersistentEdit(runtime_logger_, req, "new_word_resolve", report);
  if (outcome == learning::AutoWordSaveOutcome::NotFound) return fail(ipc::kNewWordErrorNotFound);
  // A repeated click is a success that changed nothing, and it does not
  // rewrite the file.
  if (outcome == learning::AutoWordSaveOutcome::Unchanged) {
    res.ok = true;
    return reply();
  }
  if (outcome == learning::AutoWordSaveOutcome::SaveFailed) {
    return fail(ipc::kNewWordErrorSaveFailed);
  }
  // DEV-1584: a newly confirmed English word joins the English dictionary;
  // DEV-1596: a newly rejected one leaves it when a runtime upsert added it.
  // The reply does not depend on either; a failure is logged by the engine.
  if (target == learning::AutoWordState::Confirmed) {
    engine_->RegisterConfirmedEnglishWord(parsed->surface, "new_word_resolve", english_epoch);
  } else {
    engine_->RetractRejectedEnglishWord(parsed->surface, "new_word_resolve");
  }
  res.ok = true;
  res.changed = true;
  return reply();
}

namespace {
// Store names on the wire; an unknown name fails the whole request.
std::optional<std::vector<LearningDataStore>> ParseStoreList(
    const std::vector<std::string>& names) {
  std::vector<LearningDataStore> stores;
  for (const auto& name : names) {
    const auto store = ParseLearningDataStore(name);
    if (!store) return std::nullopt;
    stores.push_back(*store);
  }
  return stores;
}
}  // namespace

std::optional<ipc::Envelope> Dispatcher::HandleListLearningEntries(const ipc::Envelope& req) {
  ipc::ListLearningEntriesResponse res;
  const auto parsed = ipc::ParseListLearningEntriesRequest(req.payload_json);
  const auto store = parsed ? ParseLearningDataStore(parsed->store) : std::nullopt;
  if (!store) {
    res.ok = false;
    res.error = std::string(ipc::kLearningDataErrorInvalidRequest);
    return MakeResponse(req, ipc::BuildListLearningEntriesResponse(res));
  }
  auto page = engine_->ListLearningEntries(*store, parsed->query, parsed->offset, parsed->limit);
  res.total = page.total;
  for (auto& entry : page.entries) {
    res.entries.push_back({std::move(entry.id), std::move(entry.channel), std::move(entry.reading),
                           std::move(entry.surface), entry.weight, entry.last_updated_epoch_sec,
                           std::move(entry.tags), std::move(entry.metadata)});
  }
  return MakeResponse(req, ipc::BuildListLearningEntriesResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleForgetLearningEntry(const ipc::Envelope& req) {
  ipc::ForgetLearningEntryResponse res;
  const auto reply = [&]() {
    return MakeResponse(req, ipc::BuildForgetLearningEntryResponse(res));
  };
  const auto parsed = ipc::ParseForgetLearningEntryRequest(req.payload_json);
  const auto store = parsed ? ParseLearningDataStore(parsed->store) : std::nullopt;
  if (!store) {
    res.ok = false;
    res.error = std::string(ipc::kLearningDataErrorInvalidRequest);
    return reply();
  }
  // The TIP names the pair, which addresses the kana learning channel only;
  // the settings app names an id from the list.
  if (parsed->id.empty() && *store != LearningDataStore::Learning) {
    res.ok = false;
    res.error = std::string(ipc::kLearningDataErrorInvalidRequest);
    return reply();
  }
  const auto outcome = parsed->id.empty()
                           ? engine_->ForgetLearningPair(parsed->reading, parsed->surface)
                           : engine_->ForgetLearningEntry(*store, parsed->id);
  res.removed = outcome == InferenceEngine::ForgetOutcome::Forgotten;
  if (outcome == InferenceEngine::ForgetOutcome::SaveFailed) {
    res.ok = false;
    res.error = std::string(ipc::kLearningDataErrorSaveFailed);
  }
  return reply();
}

std::optional<ipc::Envelope> Dispatcher::HandleResetLearningStore(const ipc::Envelope& req) {
  ipc::ResetLearningStoreResponse res;
  const auto parsed = ipc::ParseResetLearningStoreRequest(req.payload_json);
  const auto store = parsed ? ParseLearningDataStore(parsed->store) : std::nullopt;
  if (!store) {
    res.ok = false;
    res.error = std::string(ipc::kLearningDataErrorInvalidRequest);
  } else {
    switch (engine_->ResetLearningStore(*store)) {
      case InferenceEngine::ResetOutcome::Reset:
        break;
      case InferenceEngine::ResetOutcome::Unavailable:
        res.ok = false;
        res.error = std::string(ipc::kLearningDataErrorStoreUnavailable);
        break;
      case InferenceEngine::ResetOutcome::SaveFailed:
        res.ok = false;
        res.error = std::string(ipc::kLearningDataErrorSaveFailed);
        break;
    }
  }
  return MakeResponse(req, ipc::BuildResetLearningStoreResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleQueryPersona(const ipc::Envelope& req) {
  ipc::QueryPersonaResponse res;
  if (const auto snapshot = engine_->CurrentPersona()) {
    res.polite_ratio = snapshot->persona.polite_ratio;
    res.casual_ratio = snapshot->persona.casual_ratio;
    res.technical_ratio = snapshot->persona.technical_ratio;
    res.kaomoji_ratio = snapshot->persona.kaomoji_ratio;
    res.sample_count = snapshot->persona.sample_count;
    res.computed_at_epoch_sec = snapshot->computed_at_epoch_sec;
  } else {
    res.ok = false;
    res.error = std::string(ipc::kLearningDataErrorStoreUnavailable);
  }
  return MakeResponse(req, ipc::BuildQueryPersonaResponse(res));
}

// rich-features-spec X-3-6 (DEV-1532). The AI backend runs under the same
// settings and consent as ai-cleanup, but only on the remote path; without it
// the answer is "unsupported" rather than a guess from a weaker model.
std::optional<ipc::Envelope> Dispatcher::HandleDetectAnomalies(const ipc::Envelope& req) {
  ipc::DetectAnomaliesResponse res;
  const auto fail = [&](std::string_view error) {
    res.ok = false;
    res.error = std::string(error);
    return MakeResponse(req, ipc::BuildDetectAnomaliesResponse(res));
  };
  const auto parsed = ipc::ParseDetectAnomaliesRequest(req.payload_json);
  if (!parsed) return fail(ipc::kLearningDataErrorInvalidRequest);
  auto privacy =
      settings_store_ ? std::optional{settings_store_->LockPrivacyPolicy()} : std::nullopt;
  const bool host_blocked =
      privacy && (privacy->policy.secure || !privacy->policy.learning_allowed);
  // Text from a secure or learning-disallowed context never leaves the Host.
  if (parsed->secure || !parsed->learning_allowed || host_blocked) {
    return fail(ipc::kAnomalyErrorBlocked);
  }
  privacy.reset();

  AiBackendOptions options;
  // Without settings there is no consent to send anything anywhere.
  core::AiPrivacy ai_privacy{false, false};
  {
    std::lock_guard lock(*config_.update_config_mutex);
    if (settings_store_) {
      const auto& settings = settings_store_->settings();
      options.backend = settings.ai_backend;
      options.endpoint = settings.open_ai_api_endpoint;
      options.api_key = settings.open_ai_api_key;
      options.model = settings.open_ai_model;
      options.timeout_ms = settings.open_ai_timeout_ms;
      ai_privacy = settings.ai_privacy;
    }
  }
  AiTransformRequest transform;
  transform.task = AiTask::Anomalies;
  transform.text = parsed->text;
  const auto persona = engine_->CurrentPersona();
  transform.prompt = AnomalyPersonaHint(persona ? std::optional{persona->persona} : std::nullopt);
  transform.ai_allowed = ai_privacy.ai;
  transform.external_allowed = ai_privacy.external;

  auto cancel = scheduler_->TrackCancellation(client_id_, req.request_id);
  if (!cancel) return fail(ipc::kAnomalyErrorBackendFailed);
  RequestCompletionGuard completion(scheduler_, client_id_, req.request_id);
  const auto result = config_.ai_backend->Transform(transform, options, cancel.get());
  const bool canceled = cancel->load(std::memory_order_acquire);
  completion.Complete();
  if (canceled || result.error_class == AiErrorClass::Canceled) return std::nullopt;
  if (!result.ok) {
    // No API key is a setting the user has to make, as Disabled is.
    const bool unavailable = result.error_class == AiErrorClass::Disabled ||
                             result.error_class == AiErrorClass::BlockedBySecure ||
                             (result.error_class == AiErrorClass::Auth && options.api_key.empty());
    return fail(unavailable ? ipc::kLearningDataErrorUnsupported : ipc::kAnomalyErrorBackendFailed);
  }
  auto findings = ParseAnomalyFindings(parsed->text, result.result, parsed->max_findings);
  if (!findings) return fail(ipc::kAnomalyErrorBackendFailed);
  res.findings = std::move(*findings);
  return MakeResponse(req, ipc::BuildDetectAnomaliesResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleExportLearningData(const ipc::Envelope& req) {
  ipc::ExportLearningDataResponse res;
  const auto reply = [&]() { return MakeResponse(req, ipc::BuildExportLearningDataResponse(res)); };
  const auto parsed = ipc::ParseExportLearningDataRequest(req.payload_json);
  const auto stores = parsed ? ParseStoreList(parsed->stores) : std::nullopt;
  if (!stores) {
    res.error = std::string(ipc::kLearningDataErrorInvalidRequest);
    return reply();
  }
  // settings.redacted.json is a follow-up; refuse rather than drop it silently.
  if (parsed->include_settings) {
    res.error = std::string(ipc::kLearningDataErrorUnsupported);
    return reply();
  }
  BackupManifest meta;
  meta.created_at = std::format(
      "{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
  meta.app_version = config_.host_version;
  meta.host_version = config_.host_version;
  const auto result = engine_->ExportLearningData(*stores, core::Utf8Path(parsed->destination_path),
                                                  parsed->encrypt, meta);
  if (result.error != BackupError::None) {
    res.error = std::string(BackupErrorCode(result.error));
    return reply();
  }
  res.status = "success";
  res.file_size_bytes = result.file_size;
  res.encrypted = result.manifest.encrypted;
  for (const auto& item : result.manifest.items) {
    res.items.push_back({item.name, item.file, item.count, item.sha256});
  }
  return reply();
}

std::optional<ipc::Envelope> Dispatcher::HandleImportLearningData(const ipc::Envelope& req) {
  ipc::ImportLearningDataResponse res;
  const auto reply = [&]() { return MakeResponse(req, ipc::BuildImportLearningDataResponse(res)); };
  const auto parsed = ipc::ParseImportLearningDataRequest(req.payload_json);
  const auto stores = parsed ? ParseStoreList(parsed->stores) : std::nullopt;
  const auto policy =
      parsed ? learning::ParseImportConflictPolicy(parsed->conflict_resolution) : std::nullopt;
  if (!stores || !policy) {
    res.error = std::string(ipc::kLearningDataErrorInvalidRequest);
    return reply();
  }
  const auto result =
      engine_->ImportLearningData(*stores, core::Utf8Path(parsed->source_path), *policy);
  if (result.error != BackupError::None) {
    res.error = std::string(BackupErrorCode(result.error));
    return reply();
  }
  res.status = "success";
  for (const auto& [name, counts] : result.counts) {
    res.imported_counts[name] = counts.imported;
    res.skipped_counts[name] = counts.skipped;
    res.conflict_counts[name] = counts.conflicts;
  }
  return reply();
}

std::optional<ipc::Envelope> Dispatcher::HandleQueryBatchConversion(const ipc::Envelope& req) {
  auto parsed = ipc::ParseQueryBatchConversionRequest(req.payload_json);
  InferenceTrace trace(
      req.request_id, client_id_, req.trace_id,
      parsed && parsed->mode == "ai-cleanup" ? core::EtwBackend::Ai : core::EtwBackend::Unknown,
      parsed ? parsed->reading.size() : 0, runtime_logger_);
  if (!parsed) {
    trace.ProtocolError();
    DispatcherLogger(runtime_logger_)
        .Log(logging::RuntimeLogLevel::Error, "invalid_batch_request",
             {{"request_id", req.request_id},
              {"error_code", logging::RuntimeLogSafeText("protocol")}});
    core::EtwLogger::LogError(core::EtwModule::Host, core::EtwErrorCode::Protocol,
                              kGenericFailureHresult);
    ipc::QueryBatchConversionResponse res;
    return MakeResponse(req, ipc::BuildQueryBatchConversionResponse(res));
  }

  const auto queue_start =
      runtime_logger_ && runtime_logger_->enabled() && ipc::IsValidTraceId(req.trace_id)
          ? std::optional{std::chrono::steady_clock::now()}
          : std::nullopt;
  auto cancel = scheduler_->TrackCancellation(client_id_, req.request_id);
  if (!cancel) {
    LogHostQueueWait(runtime_logger_, req, queue_start, "error");
    // Preserve the wire response while recording the capacity rejection accurately.
    ipc::QueryBatchConversionResponse res;
    res.canceled = true;
    return MakeResponse(req, ipc::BuildQueryBatchConversionResponse(res));
  }
  scheduler_->MarkLatest(client_id_, req.request_id);
  LogHostQueueWait(runtime_logger_, req, queue_start, "ok");
  trace.WatchCancellation(cancel);
  RequestCompletionGuard completion(scheduler_, client_id_, req.request_id);

  ipc::QueryBatchConversionResponse res;
  bool suppress_learning = false;
  if (parsed->mode == "ai-cleanup") {
    AiBackendOptions options;
    core::AiPrivacy privacy{true, true};
    {
      std::lock_guard lock(*config_.update_config_mutex);
      if (settings_store_) {
        const auto& settings = settings_store_->settings();
        options.backend = settings.ai_backend;
        options.endpoint = settings.open_ai_api_endpoint;
        options.api_key = settings.open_ai_api_key;
        options.model = settings.open_ai_model;
        options.timeout_ms = settings.open_ai_timeout_ms;
        options.include_context = settings.include_context_in_ai_transform;
        privacy = settings.ai_privacy;
      }
    }
    AiTransformRequest transform;
    if (!parsed->ai_backend.empty()) options.backend = parsed->ai_backend;
    transform.task = AiTask::Cleanup;
    if (parsed->raw_romaji.empty()) options.backend = "none";
    transform.text = parsed->reading;
    transform.raw_romaji = parsed->raw_romaji;
    transform.auto_punctuation = parsed->auto_punctuation;
    transform.ai_allowed = privacy.ai && parsed->ai_allowed;
    suppress_learning = !transform.ai_allowed;
    transform.external_allowed = privacy.external && parsed->external_ai_allowed;
    const auto phase_start = std::chrono::steady_clock::now();
    AiTransformResult result;
    try {
      result = config_.ai_backend->Transform(
          transform, options, cancel.get(),
          [this](const AiTransformRequest& local, const std::atomic<bool>* flag,
                 AiDeadline deadline) { return engine_->TransformLocal(local, flag, deadline); });
    } catch (...) {
      core::EtwLogger::LogInferencePhase(
          req.request_id, core::EtwPhase::AiTransform, core::EtwBackend::Ai,
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start)
              .count(),
          cancel->load(std::memory_order_acquire) ? core::EtwResult::Cancelled
                                                  : core::EtwResult::Failed,
          trace.context()->client);
      throw;
    }
    const auto phase_result =
        result.ok                                      ? core::EtwResult::Success
        : result.error_class == AiErrorClass::Canceled ? core::EtwResult::Cancelled
        : result.error_class == AiErrorClass::Timeout  ? core::EtwResult::Timeout
                                                       : core::EtwResult::Failed;
    core::EtwLogger::LogInferencePhase(
        req.request_id, core::EtwPhase::AiTransform, core::EtwBackend::Ai,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start)
            .count(),
        phase_result, trace.context()->client);
    if (!result.ok && result.error_class != AiErrorClass::Canceled) {
      DispatcherLogger(runtime_logger_)
          .Log(logging::RuntimeLogLevel::Error, "ai_cleanup_fallback",
               {{"request_id", req.request_id},
                {"error_code", logging::RuntimeLogSafeText("business")},
                {"error_class", static_cast<uint64_t>(result.error_class)}});
      core::EtwLogger::LogError(core::EtwModule::Host, core::EtwErrorCode::Business,
                                kGenericFailureHresult);
      res.error_class = AiErrorClassToWire(result.error_class);
    }
    if (result.ok) {
      ipc::CandidateField candidate;
      candidate.reading = parsed->reading;
      candidate.surface = result.result;
      candidate.source = "llm";
      res.full_surface = candidate.surface;
      res.segments.push_back({parsed->reading, {std::move(candidate)}});
    }
  }
  if (res.segments.empty()) {
    const auto chunks = core::SplitBatchConversion(parsed->reading);
    // M60: only a batch that stays one segment pairs raw_romaji with a
    // segment; a split reading has no per-segment romaji to offer.
    // The romaji is the user's input: a secure (or unmarked) batch never uses
    // it (mixed-english-auto-conversion-spec section 9.4).
    const bool english = parsed->mode == "neural" && !parsed->secure &&
                         parsed->english_candidates && !parsed->raw_romaji.empty() &&
                         chunks.size() == 1;
    for (const auto& reading : chunks) {
      if (cancel->load(std::memory_order_acquire)) break;
      auto candidates = engine_->QueryCandidates(reading, "", NowSec(), cancel.get(),
                                                 parsed->max_candidates, false, trace.context());
      if (parsed->max_candidates > 0 && candidates.size() > parsed->max_candidates) {
        candidates.resize(parsed->max_candidates);
      }
      // English goes after the truncation, as in QueryCandidates (section 4.3),
      // and never into an empty list, where it would take the first slot.
      if (english && !candidates.empty() && !cancel->load(std::memory_order_acquire)) {
        auto english_result = engine_->QueryEnglishCandidates(parsed->raw_romaji, NowSec());
        PlaceEnglishCandidates(candidates, std::move(english_result.candidates),
                               english_result.intent, engine_->config().english.promote_threshold);
      }
      ipc::BatchConversionSegment segment;
      segment.reading = reading;
      for (auto& candidate : candidates) segment.candidates.push_back(ToField(candidate));
      if (segment.candidates.empty()) {
        ipc::CandidateField fallback;
        fallback.reading = reading;
        fallback.surface = reading;
        fallback.source = "fallback";
        segment.candidates.push_back(std::move(fallback));
      }
      // Preserve secure/unknown-input learning suppression even when host and
      // TIP settings snapshots differ. The surface still comes from neural.
      if (suppress_learning)
        for (auto& candidate : segment.candidates) candidate.source = "privacy-fallback";
      res.full_surface += segment.candidates.front().surface;
      res.segments.push_back(std::move(segment));
    }
  }
  if (cancel->load(std::memory_order_acquire)) {
    res = {};
    res.canceled = true;
  }
  completion.Complete();
  uint64_t candidate_count = 0;
  for (const auto& segment : res.segments) candidate_count += segment.candidates.size();
  trace.Finish(res.canceled ? core::EtwResult::Cancelled : core::EtwResult::Success,
               candidate_count);
  return MakeResponse(req, ipc::BuildQueryBatchConversionResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleReverseConvert(const ipc::Envelope& req) {
  ipc::ReverseConvertResponse response;
  if (const auto parsed = ipc::ParseReverseConvertRequest(req.payload_json)) {
    response.reading = engine_->ReverseConvert(parsed->surface, NowSec());
    response.confidence = response.reading.empty() ? 0.0 : 1.0;
  }
  return MakeResponse(req, ipc::BuildReverseConvertResponse(response));
}

void Dispatcher::HandleCancel(const ipc::Envelope& req) {
  if (auto parsed = ipc::ParseCancel(req.payload_json)) {
    scheduler_->Cancel(client_id_, parsed->target_request_id);
  }
}

std::optional<ipc::Envelope> Dispatcher::HandleCommitObservation(const ipc::Envelope& req) {
  auto privacy =
      settings_store_ ? std::optional{settings_store_->LockPrivacyPolicy()} : std::nullopt;
  ipc::CommitObservationResponse res;
  if (auto parsed = ipc::ParseCommitObservationRequest(req.payload_json);
      parsed &&
      LearningAllowed(parsed->secure, parsed->learning_allowed,
                      privacy && (privacy->policy.secure || !privacy->policy.learning_allowed))) {
    // A duplicate resend is answered ok=true: the observation is already
    // recorded, so the TIP must stop retrying it (DEV-554).
    if (IsEnglishObservation(parsed->reading, parsed->chosen.tag)) {
      // M60 section 6.4: never the kana store, auto-word mining or the converter.
      engine_->CommitEnglishObservation(parsed->reading, parsed->chosen.surface, NowSec(),
                                        parsed->observation_id);
    } else if (engine_->CommitObservation(
                   parsed->reading, parsed->chosen.surface, NowSec(), parsed->observation_id,
                   parsed->app ? parsed->app->process_name : std::string(), parsed->left_context)) {
      // M63-B (mixed-english-auto-conversion-spec section 11): the English the
      // composition was shown first, turned down.
      engine_->RecordRejectedEnglish(parsed->shown, parsed->chosen.surface, NowSec());
    }
    res.ok = true;
  } else {
    res.ok = false;
  }
  return MakeResponse(req, ipc::BuildCommitObservationResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleCommitCorrection(const ipc::Envelope& req) {
  auto privacy =
      settings_store_ ? std::optional{settings_store_->LockPrivacyPolicy()} : std::nullopt;
  ipc::CommitObservationResponse res;
  // Same privacy gate as CommitObservation (user-learning-enhancement-spec
  // section 4); a duplicate resend is answered ok=true without re-recording.
  if (auto parsed = ipc::ParseCommitCorrectionRequest(req.payload_json);
      parsed &&
      LearningAllowed(parsed->secure, parsed->learning_allowed,
                      privacy && (privacy->policy.secure || !privacy->policy.learning_allowed))) {
    engine_->CommitCorrection(parsed->reading, parsed->rejected_surface, parsed->selected_surface,
                              NowSec(), parsed->observation_id,
                              parsed->app ? parsed->app->process_name : std::string(),
                              parsed->left_context);
    res.ok = true;
  }
  return MakeResponse(req, ipc::BuildCommitObservationResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleAddUserWord(const ipc::Envelope& req) {
  // DEV-1572: the edit's wait budget runs from here.
  UserDictionaryEditReport report;
  UserDictionaryEditOptions options;
  options.report = &report;
  ipc::AddUserWordResponse res;
  if (!user_dict_) {
    res.ok = false;
    res.error = std::string(ipc::kUserDictErrorUnavailable);
    return MakeResponse(req, ipc::BuildAddUserWordResponse(res));
  }
  if (auto parsed = ipc::ParseAddUserWordRequest(req.payload_json)) {
    learning::UserWord w;
    w.word = parsed->word;
    w.ruby = parsed->ruby;
    w.cid = parsed->cid;
    w.mid = parsed->mid;
    w.value = parsed->value;
    res.ok = engine_->AddUserWord(w, options);
    if (!res.ok) res.error = UserDictionaryEditResultName(report.result);
    LogPersistentEdit(runtime_logger_, req, "user_dict_add", report);
  } else {
    res.error = std::string(ipc::kUserDictErrorInvalidRequest);
  }
  return MakeResponse(req, ipc::BuildAddUserWordResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleRemoveUserWord(const ipc::Envelope& req) {
  UserDictionaryEditReport report;
  UserDictionaryEditOptions options;
  options.report = &report;
  ipc::RemoveUserWordResponse res;
  if (!user_dict_) {
    res.ok = false;
    res.error = std::string(ipc::kUserDictErrorUnavailable);
    return MakeResponse(req, ipc::BuildRemoveUserWordResponse(res));
  }
  if (auto parsed = ipc::ParseRemoveUserWordRequest(req.payload_json)) {
    res.ok = engine_->RemoveUserWord(parsed->word, parsed->ruby, options);
    if (!res.ok) res.error = UserDictionaryEditResultName(report.result);
    LogPersistentEdit(runtime_logger_, req, "user_dict_remove", report);
  } else {
    res.error = std::string(ipc::kUserDictErrorInvalidRequest);
  }
  return MakeResponse(req, ipc::BuildRemoveUserWordResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleUpdateConfig(const ipc::Envelope& req) {
  ipc::UpdateConfigResponse res;
  if (!settings_store_) {
    res.ok = false;
    res.error = "settings store not configured";
    return MakeResponse(req, ipc::BuildUpdateConfigResponse(res));
  }

  std::lock_guard<std::mutex> lock(*config_.update_config_mutex);
  const auto load_result = settings_store_->Reload();
  core::CrashReporting::SetConsent(load_result.settings.crash_report_consent == "local"
                                       ? core::CrashConsent::Local
                                       : core::CrashConsent::Off);
  if (load_result.status == SettingsLoadStatus::Invalid) {
    res.ok = false;
    res.error = load_result.error.value_or("invalid settings.json");
    return MakeResponse(req, ipc::BuildUpdateConfigResponse(res));
  }

  if (load_result.settings.backend_preference_conflict) {
    DispatcherLogger(runtime_logger_)
        .Log(logging::RuntimeLogLevel::Warn, "backend_preference_conflict",
             {{"request_id", req.request_id},
              {"error_code", logging::RuntimeLogSafeText("business")}});
  }
  const auto auto_backend = config_.auto_backend ? config_.auto_backend() : config_.default_backend;
  auto next_config =
      ApplyRuntimeSettingsToEngineConfig(engine_->config(), load_result.settings, auto_backend);
  if (config_.override_backend) {
    next_config.backend = *config_.override_backend;
  }
  const bool safe_mode = load_result.settings.safe_mode.enabled;
  if (config_.override_model_path && !safe_mode) {
    next_config.model_path = *config_.override_model_path;
  }
  // SafeMode ends only here, when the user has turned the flag off (section
  // 8.5.1); a flag that appears on disk is followed as it stands.
  if (!safe_mode && engine_->health_state() == HealthState::SafeMode) {
    (void)engine_->ApplyHealthEvent(HealthEvent::SafeModeCleared);
  } else if (safe_mode && engine_->health_state() != HealthState::SafeMode) {
    engine_->RestoreHealthState(HealthState::SafeMode);
  }
  engine_->ApplyConfig(next_config);
  if (config_.on_config_applied) {
    config_.on_config_applied(next_config);
  }
  if (config_.on_network_policy_applied) {
    config_.on_network_policy_applied(!load_result.settings.offline_mode);
  }
  ModelLoadOptions load_options{next_config.model_path, next_config.backend,
                                next_config.n_gpu_layers, next_config.inference_threads};
  load_options.resolve_prompt_format =
      CatalogPromptFormatResolver(config_.catalog_matcher, next_config.model_path);
  const auto model_result = engine_->LoadModelWithResult(load_options);

  res.ok = model_result.ok;
  if (load_result.error) {
    res.error = *load_result.error;
  } else if (!model_result.ok) {
    res.error = model_result.error.value_or("model load failed");
  }
  return MakeResponse(req, ipc::BuildUpdateConfigResponse(res));
}

// M45 (docs/model-management-spec.md section 4.1). Read-only: enumerates the
// models directory and never loads anything, so the live backend is untouched.
std::optional<ipc::Envelope> Dispatcher::HandleListModels(const ipc::Envelope& req) {
  ipc::ListModelsResponse res;
  const auto parsed = ipc::ParseListModelsRequest(req.payload_json);
  const auto directory =
      parsed ? ResolveModelListingDirectory(parsed->directory, config_.models_dir) : std::nullopt;
  if (!parsed || !directory) {
    res.ok = false;
    res.error = !parsed                      ? "invalid_request"
                : config_.models_dir.empty() ? "models_dir_unavailable"
                                             : "directory_outside_models_root";
    return MakeResponse(req, ipc::BuildListModelsResponse(res));
  }
  try {
    ModelScanOptions options;
    options.compute_sha256 = parsed->compute_sha256;
    const auto health = engine_->health_snapshot();
    const auto loaded_path = core::Utf8Path(health.model_path);
    for (const auto& entry : ScanModelDirectory(*directory, options)) {
      std::error_code ec;
      const bool current =
          !health.model_path.empty() && std::filesystem::equivalent(entry.path, loaded_path, ec);
      std::string status = "not_loaded";
      std::string error;
      if (current && health.model_loaded) {
        status = "success";
      } else if (current && health.last_error) {
        status = "failed";
        error = "load_failed";
      }
      res.models.push_back(ToListedModel(entry, std::move(status), std::move(error)));
    }
    if (parsed->include_catalog && config_.catalog_matcher) {
      res.catalog = CatalogEntries(*config_.catalog_matcher);
      for (auto& model : res.models) {
        if (model.format != "gguf" || !model.valid) continue;
        if (const auto entry = config_.catalog_matcher->Match(model.path)) {
          model.catalog_id = entry->id;
        }
      }
    }
  } catch (...) {
    res.models.clear();
    res.catalog.reset();
    res.ok = false;
    res.error = "scan_failed";
  }
  return MakeResponse(req, ipc::BuildListModelsResponse(res));
}

// DEV-1821 (model-management-spec section 4.4): starts the fetch on the shared service and
// answers at once with its state; QueryCatalogFetch follows its progress.
std::optional<ipc::Envelope> Dispatcher::HandleFetchCatalogModel(const ipc::Envelope& req) {
  ipc::CatalogFetchResponse res;
  std::optional<CatalogFetchSnapshot> state;
  const auto parsed = ipc::ParseFetchCatalogModelRequest(req.payload_json);
  if (!parsed) {
    res.ok = false;
    res.error = "invalid_request";
  } else if (!config_.catalog_fetch) {
    res.ok = false;
    res.error = "catalog_unavailable";
  } else {
    // The state comes from the start itself: a fetch another connection starts right after
    // must not be reported as this one.
    state.emplace();
    switch (config_.catalog_fetch->Start(parsed->model_id, &*state)) {
      case CatalogFetchStartResult::Started:
        break;
      case CatalogFetchStartResult::Busy:
        res.ok = false;
        res.error = "busy";
        break;
      case CatalogFetchStartResult::UnknownModelId:
        res.ok = false;
        res.error = "unknown_model_id";
        break;
      case CatalogFetchStartResult::Unavailable:
        res.ok = false;
        res.error = "catalog_unavailable";
        break;
    }
  }
  if (!state && config_.catalog_fetch) state = config_.catalog_fetch->Snapshot();
  if (state) FillCatalogFetchResponse(*state, res);
  return MakeResponse(req, ipc::BuildCatalogFetchResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleQueryCatalogFetch(const ipc::Envelope& req) {
  ipc::CatalogFetchResponse res;
  if (!config_.catalog_fetch) {
    res.ok = false;
    res.error = "catalog_unavailable";
  } else {
    FillCatalogFetchResponse(config_.catalog_fetch->Snapshot(), res);
  }
  return MakeResponse(req, ipc::BuildCatalogFetchResponse(res));
}

// M45 section 4.2: a separate engine, synchronous on this connection, bounded
// by kBenchmarkBudget. The live engine keeps serving other connections.
std::optional<ipc::Envelope> Dispatcher::HandleBenchmarkModel(const ipc::Envelope& req) {
  ipc::BenchmarkModelResponse res;
  const auto parsed = ipc::ParseBenchmarkModelRequest(req.payload_json);
  if (!parsed) {
    res.error = "invalid_request";
  } else if (SafeModeEnabled()) {
    res.backend = parsed->backend;
    res.error = "safe_mode";
  } else if (const auto resolved = ResolveModelListingDirectory(parsed->path, config_.models_dir);
             !resolved) {
    // Same boundary as ListModels: only models the settings app can list.
    res.backend = parsed->backend;
    res.error = config_.models_dir.empty() ? "models_dir_unavailable" : "path_outside_models_root";
  } else {
    ModelBenchmarkOptions options;
    options.base_config = engine_->config();
    try {
      // Load exactly the path that was checked, not the raw request spelling.
      auto request = *parsed;
      request.path = core::PathToUtf8(*resolved);
      // RunModelBenchmark owns the slot through child-process reclamation.
      res = RunModelBenchmark(request, options);
    } catch (...) {
      res = ipc::BenchmarkModelResponse{};
      res.backend = parsed->backend.empty() ? "cpu" : parsed->backend;
      res.error = "benchmark_failed";
    }
    // The store excludes admission rejections, including worker-side busy.
    if (settings_store_) {
      try {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        settings_store_->PersistBenchmarkResult(core::PathToUtf8(*resolved), FormatRfc3339Utc(now),
                                                res);
      } catch (...) {
        // History is best-effort; preserve the benchmark response on I/O failure.
      }
    }
  }
  return MakeResponse(req, ipc::BuildBenchmarkModelResponse(res));
}

}  // namespace azookey::host
