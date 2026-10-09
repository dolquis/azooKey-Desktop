#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace azookey::ipc {

enum class MessageType {
  Handshake,
  LoadModel,
  QueryCandidates,
  QueryLiveConversion,
  QueryBatchConversion,
  QueryPredictions,
  QueryCorrections,
  Cancel,
  CommitObservation,
  CommitCorrection,
  AddUserWord,
  UpdateUserWord,
  RemoveUserWord,
  UpdateConfig,
  Ping,
  Health,
  QueryDiagnostics,
  CommitSegmentsObservation,
  ObserveTypo,
  ListNewWordCandidates,
  ResolveNewWord,
  ReverseConvert,
  // M45 model management (DEV-1191).
  ListModels,
  BenchmarkModel,
  // M49 learning data management (DEV-1190).
  ListLearningEntries,
  ForgetLearningEntry,
  ExportLearningData,
  ImportLearningData,
  // DEV-1208 settings app: whole-store reset (learning-data-management-spec
  // section 4.6) and the persona view (rich-features-spec X-2-7).
  ResetLearningStore,
  QueryPersona,
  // DEV-1532 batch correction view (rich-features-spec X-3-6).
  DetectAnomalies,
  Unknown,
};

// Current wire generation of the Envelope. Field additions are backward
// compatible and do not bump this; only breaking changes (field removal or
// type change) require a bump. Deserialize rejects envelopes outside the
// supported range so a future breaking generation is dropped rather than
// silently misinterpreted.
constexpr int kEnvelopeVersion = 1;

struct Envelope {
  int version{kEnvelopeVersion};
  uint64_t request_id{};
  std::string trace_id;
  MessageType type{MessageType::Unknown};
  std::string payload_json;
};

// Returns std::nullopt when payload_json is non-empty but not valid JSON, so a
// malformed payload surfaces as a serialization failure instead of being
// silently embedded as a raw string.
std::optional<std::string> Serialize(const Envelope& env);
std::optional<Envelope> Deserialize(const std::string& json);

std::string TypeToString(MessageType type);
MessageType TypeFromString(const std::string& value);

std::optional<std::vector<uint8_t>> EncodeLengthPrefixed(const std::string& json);
std::optional<std::string> DecodeLengthPrefixed(const std::vector<uint8_t>& bytes);

}  // namespace azookey::ipc
