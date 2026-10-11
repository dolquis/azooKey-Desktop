#pragma once

#include <cstdint>
#include <string>

namespace azookey::tsf {

// Dynamic punctuation (docs/dynamic-punctuation-spec.md section 8) as the TIP
// reads it. The Host never learns the timing: the TIP encodes it per request in
// QueryLiveConversionRequest::auto_punctuation (section 7.1.1).
struct TipDynamicPunctuationSettings {
  bool enabled{false};
  std::string style{"ja"};  // "ja" or "fullwidth_latin".
  bool eager{false};        // dynamicPunctuationStability == "eager".
  uint32_t idle_ms{400};
};

inline constexpr uint32_t kDefaultDynamicPunctuationIdleMs = 400;
// Section 7.3: the final request before a commit is bounded; "about 200 ms" is
// the spec's guideline and the value is implementation-defined.
inline constexpr uint32_t kFinalPunctuationRequestTimeoutMs = 200;

// What produced a live conversion request (section 7.1.1).
enum class LiveRequestTiming {
  Edit,   // A typed key or Backspace.
  Idle,   // The onPause idle timer expired.
  Final,  // The evaluation that precedes a commit.
};

// Section 3: the feature needs liveConversion; with it off nothing changes.
inline bool DynamicPunctuationActive(const TipDynamicPunctuationSettings& settings,
                                     bool live_conversion) {
  return live_conversion && settings.enabled;
}

// The auto_punctuation value of one request (section 7.1.1 table).
inline bool AutoPunctuationFor(const TipDynamicPunctuationSettings& settings, bool live_conversion,
                               LiveRequestTiming timing) {
  if (!DynamicPunctuationActive(settings, live_conversion)) return false;
  return timing != LiveRequestTiming::Edit || settings.eager;
}

// Section 4.3.1: only onPause needs a timer to drive the insertion.
inline bool UsesIdleTimer(const TipDynamicPunctuationSettings& settings, bool live_conversion) {
  return DynamicPunctuationActive(settings, live_conversion) && !settings.eager;
}

// Section 7.3: a commit needs a final auto_punctuation request unless the
// displayed result already carries the punctuation (eager always does, and an
// onPause result that came from an idle or final request and has not changed).
inline bool NeedsFinalPunctuationRequest(const TipDynamicPunctuationSettings& settings,
                                         bool live_conversion, bool displayed_has_punctuation) {
  return UsesIdleTimer(settings, live_conversion) && !displayed_has_punctuation;
}

}  // namespace azookey::tsf
