#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

#include "azookey/host/RequestScheduler.h"

namespace azookey::logging {
class RuntimeLogger;
}

namespace azookey::host {

// Anomalies: proofread-view-spec §1 detection for the view of proofread-view-spec §2. It and
// Transform (M16, the selected text) are remote-only: the local path has no
// model that can do them.
enum class AiTask { Transform, Cleanup, Lint, Anomalies };
enum class AiErrorClass {
  None,
  Auth,
  RateLimit,
  ServerError,
  Network,
  Timeout,
  Parse,
  BlockedBySecure,
  Disabled,
  Canceled,
  KeyReentry
};
struct AiTransformRequest {
  AiTask task{AiTask::Transform};
  std::string mode{"rewrite"};
  std::string prompt;
  std::string text;
  std::string raw_romaji;
  std::string left_context;
  bool auto_punctuation{false};
  bool ai_allowed{false};
  bool external_allowed{false};
};
struct AiTransformResult {
  bool ok{false};
  std::string result;
  AiErrorClass error_class{AiErrorClass::None};
};
struct AiBackendOptions {
  std::string backend{"none"};
  std::string endpoint{"https://api.openai.com/v1"};
  std::string api_key;
  std::string model{"gpt-4o-mini"};
  int timeout_ms{30000};
  bool include_context{true};
};
using AiDeadline = std::chrono::steady_clock::time_point;
struct AiHttpResponse {
  unsigned status{0};
  std::string body;
  AiErrorClass error{AiErrorClass::None};
  unsigned retry_after_seconds{0};
};
using AiHttpTransport = std::function<AiHttpResponse(const AiBackendOptions&, const std::string&,
                                                     const std::atomic<bool>*, AiDeadline)>;
using AiLocalTransform = std::function<AiTransformResult(const AiTransformRequest&,
                                                         const std::atomic<bool>*, AiDeadline)>;

// One synchronous entry point; the network transport is cancellable asynchronous WinHTTP.
class AiBackend {
 public:
  // `logger` takes the default transport's HTTP failure records, so the Host
  // passes its own to keep them in its logs directory; null records none.
  explicit AiBackend(AiHttpTransport transport = {}, logging::RuntimeLogger* logger = nullptr);
  AiTransformResult Transform(AiTransformRequest request, const AiBackendOptions& options,
                              const AiCancellationFlag* cancel = nullptr,
                              const AiLocalTransform& local = {}) const;

 private:
  AiHttpTransport transport_;
};

// Failures go to ETW and, when `logger` is set, to that RuntimeLogger.
AiHttpResponse PostAiHttp(const AiBackendOptions& options, const std::string& body,
                          const std::atomic<bool>* cancel, AiDeadline deadline,
                          logging::RuntimeLogger* logger = nullptr);
std::string BuildAiInstruction(const AiTransformRequest& request);
}  // namespace azookey::host
