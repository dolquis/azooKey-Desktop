#include "azookey/host/NewWordsCli.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include "azookey/host/CliText.h"
#include "azookey/ipc/Json.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/learning/AutoWordStore.h"

namespace azookey::host {

namespace {

namespace j = ::azookey::ipc::json;

constexpr uint64_t kHandshakeRequestId = 1;
constexpr uint64_t kCommandRequestId = 2;

bool SetError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}

bool ReadOptionValue(const std::vector<std::string>& args, size_t* index, const char* flag,
                     std::string* value, std::string* error) {
  if (*index + 1 >= args.size()) {
    return SetError(error, std::string("missing value for ") + flag);
  }
  *value = args[++(*index)];
  return true;
}

const char* OpName(NewWordsCliCommand command) {
  switch (command) {
    case NewWordsCliCommand::Confirm:
      return "confirm";
    case NewWordsCliCommand::Reject:
      return "reject";
    case NewWordsCliCommand::List:
      break;
  }
  return "list";
}

std::string ListEntryJsonLine(const learning::AutoWord& word) {
  j::Object object;
  object.emplace("op", j::Value("list"));
  object.emplace("ok", j::Value(true));
  object.emplace("reading", j::Value(word.reading));
  object.emplace("surface", j::Value(word.surface));
  object.emplace("source", j::Value(std::string(learning::AutoWordSourceName(word.source))));
  object.emplace("state", j::Value(std::string(learning::AutoWordStateName(word.state))));
  object.emplace("count", j::Value(static_cast<uint64_t>(word.count)));
  object.emplace("last_seen_epoch", j::Value(word.last_seen_epoch));
  return j::Stringify(j::Value(std::move(object)));
}

std::string ListEntryTsvLine(const learning::AutoWord& word) {
  std::ostringstream oss;
  oss << SanitizeTsvCell(word.reading) << '\t' << SanitizeTsvCell(word.surface) << '\t'
      << learning::AutoWordSourceName(word.source) << '\t'
      << learning::AutoWordStateName(word.state) << '\t' << word.count << '\t'
      << word.last_seen_epoch;
  return oss.str();
}

// ok means the word ends in the requested state; changed means this run moved it.
std::string ResolveJsonLine(const NewWordsCliOptions& options, bool ok, bool changed,
                            const char* via, const std::string& error) {
  j::Object object;
  object.emplace("op", j::Value(OpName(options.command)));
  object.emplace("ok", j::Value(ok));
  object.emplace("changed", j::Value(changed));
  object.emplace("reading", j::Value(options.reading));
  object.emplace("surface", j::Value(options.surface));
  object.emplace("via", j::Value(via));
  if (!error.empty()) object.emplace("error", j::Value(error));
  return j::Stringify(j::Value(std::move(object)));
}

NewWordsCliResult ResolveResult(const NewWordsCliOptions& options, bool ok, bool changed,
                                const char* via, std::string error) {
  NewWordsCliResult result;
  result.exit_code = ok ? 0 : 1;
  result.output_lines.push_back(ResolveJsonLine(options, ok, changed, via, error));
  result.error = std::move(error);
  return result;
}

azookey::ipc::Envelope MakeEnvelope(uint64_t request_id, azookey::ipc::MessageType type,
                                    std::string payload_json) {
  azookey::ipc::Envelope envelope;
  envelope.request_id = request_id;
  envelope.trace_id = "newwords-cli";
  envelope.type = type;
  envelope.payload_json = std::move(payload_json);
  return envelope;
}

NewWordsCliResult ListDirect(const NewWordsCliOptions& options,
                             const NewWordsCliRunOptions& run_options) {
  NewWordsCliResult result;
  learning::AutoWordState state = learning::AutoWordState::Pending;
  (void)learning::ParseAutoWordState(options.state, state);
  // Reads the persisted file: the running host saves on every observation and
  // every resolve, so the file is the shared source for inspection.
  learning::AutoWordStore store(run_options.auto_word_store_path);
  if (!store.Load()) {
    result.exit_code = 1;
    result.error = "failed to load auto-word store";
    return result;
  }
  auto words = store.ListByState(state);
  // Same order as ListNewWordCandidates: most recently seen first.
  std::sort(words.begin(), words.end(),
            [](const learning::AutoWord& a, const learning::AutoWord& b) {
              if (a.last_seen_epoch != b.last_seen_epoch)
                return a.last_seen_epoch > b.last_seen_epoch;
              if (a.surface != b.surface) return a.surface < b.surface;
              return a.reading < b.reading;
            });
  if (options.format == NewWordsCliFormat::Tsv) {
    for (const auto& word : words) result.output_lines.push_back(ListEntryTsvLine(word));
    return result;
  }
  if (words.empty()) {
    j::Object object;
    object.emplace("op", j::Value("list"));
    object.emplace("ok", j::Value(true));
    result.output_lines.push_back(j::Stringify(j::Value(std::move(object))));
    return result;
  }
  for (const auto& word : words) result.output_lines.push_back(ListEntryJsonLine(word));
  return result;
}

NewWordsCliResult ResolveDirect(const NewWordsCliOptions& options,
                                const NewWordsCliRunOptions& run_options) {
  learning::AutoWordStore store(run_options.auto_word_store_path);
  if (!store.Load()) {
    return ResolveResult(options, false, false, "file", "failed to load auto-word store");
  }
  const auto target = options.command == NewWordsCliCommand::Confirm
                          ? learning::AutoWordState::Confirmed
                          : learning::AutoWordState::Rejected;
  const auto previous = store.SetState(options.surface, options.reading, target);
  if (!previous) {
    return ResolveResult(options, false, false, "file",
                         std::string(azookey::ipc::kNewWordErrorNotFound));
  }
  if (*previous == target) return ResolveResult(options, true, false, "file", {});
  if (!store.Save()) {
    return ResolveResult(options, false, false, "file",
                         std::string(azookey::ipc::kNewWordErrorSaveFailed));
  }
  return ResolveResult(options, true, true, "file", {});
}

NewWordsCliResult ResolveViaPipe(const NewWordsCliOptions& options,
                                 const NewWordsCliRunOptions& run_options) {
  azookey::ipc::NamedPipeClient client;
  const std::string pipe_name =
      run_options.pipe_name.empty() ? azookey::ipc::DefaultPipeName() : run_options.pipe_name;
  if (!client.Connect(pipe_name, run_options.connect_timeout_ms)) {
    return ResolveResult(
        options, false, false, "ipc",
        "failed to connect to running host; pass --offline to edit the file directly");
  }

  azookey::ipc::HandshakeRequest handshake;
  handshake.tip_version = "newwords-cli";
  handshake.protocol_version = 1;
  handshake.capabilities = {"newwords-cli"};
  handshake.handshake_token = run_options.handshake_token;
  if (!client.Send(MakeEnvelope(kHandshakeRequestId, azookey::ipc::MessageType::Handshake,
                                azookey::ipc::BuildHandshakeRequest(handshake)))) {
    return ResolveResult(options, false, false, "ipc", "failed to send handshake to running host");
  }
  auto handshake_resp = client.ReceiveWithTimeout(run_options.response_timeout_ms);
  if (!handshake_resp) {
    return ResolveResult(options, false, false, "ipc",
                         "timed out waiting for handshake response from running host");
  }
  auto handshake_payload = azookey::ipc::ParseHandshakeResponse(handshake_resp->payload_json);
  if (!handshake_payload || !handshake_payload->accepted) {
    return ResolveResult(options, false, false, "ipc", "handshake rejected by running host");
  }

  azookey::ipc::ResolveNewWordRequest req;
  req.surface = options.surface;
  req.reading = options.reading;
  req.action = OpName(options.command);
  if (!client.Send(MakeEnvelope(kCommandRequestId, azookey::ipc::MessageType::ResolveNewWord,
                                azookey::ipc::BuildResolveNewWordRequest(req)))) {
    return ResolveResult(options, false, false, "ipc", "failed to send request to running host");
  }
  auto resp = client.ReceiveWithTimeout(run_options.response_timeout_ms);
  if (!resp) {
    return ResolveResult(options, false, false, "ipc",
                         "timed out waiting for response from running host");
  }
  auto parsed = azookey::ipc::ParseResolveNewWordResponse(resp->payload_json);
  if (!parsed) {
    return ResolveResult(options, false, false, "ipc", "malformed response from running host");
  }
  std::string error = parsed->error.value_or(std::string());
  if (!parsed->ok && error.empty()) error = "running host rejected " + req.action;
  return ResolveResult(options, parsed->ok, parsed->changed, "ipc", std::move(error));
}

}  // namespace

std::optional<NewWordsCliOptions> ParseNewWordsCliArgs(const std::vector<std::string>& args,
                                                       std::string* error) {
  if (args.empty()) {
    SetError(error, "missing newwords subcommand");
    return std::nullopt;
  }

  NewWordsCliOptions options;
  const std::string& command = args[0];
  if (command == "list") {
    options.command = NewWordsCliCommand::List;
  } else if (command == "confirm") {
    options.command = NewWordsCliCommand::Confirm;
  } else if (command == "reject") {
    options.command = NewWordsCliCommand::Reject;
  } else {
    SetError(error, "unknown newwords subcommand: " + command);
    return std::nullopt;
  }
  const bool is_list = options.command == NewWordsCliCommand::List;

  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--offline" && !is_list) {
      options.offline = true;
    } else if (arg == "--reading" && !is_list) {
      if (!ReadOptionValue(args, &i, "--reading", &options.reading, error)) return std::nullopt;
    } else if (arg == "--surface" && !is_list) {
      if (!ReadOptionValue(args, &i, "--surface", &options.surface, error)) return std::nullopt;
    } else if (arg == "--state" && is_list) {
      if (!ReadOptionValue(args, &i, "--state", &options.state, error)) return std::nullopt;
      learning::AutoWordState state{};
      if (!learning::ParseAutoWordState(options.state, state)) {
        SetError(error, "unsupported --state: " + options.state);
        return std::nullopt;
      }
    } else if (arg == "--format" && is_list) {
      std::string value;
      if (!ReadOptionValue(args, &i, "--format", &value, error)) return std::nullopt;
      if (value == "json") {
        options.format = NewWordsCliFormat::Json;
      } else if (value == "tsv") {
        options.format = NewWordsCliFormat::Tsv;
      } else {
        SetError(error, "unsupported --format: " + value);
        return std::nullopt;
      }
    } else {
      SetError(error, "unknown newwords " + command + " argument: " + arg);
      return std::nullopt;
    }
  }

  if (!is_list && (options.reading.empty() || options.surface.empty())) {
    SetError(error, command + " requires --reading and --surface");
    return std::nullopt;
  }
  return options;
}

NewWordsCliResult RunNewWordsCli(const NewWordsCliOptions& options,
                                 const NewWordsCliRunOptions& run_options) {
  if (options.command == NewWordsCliCommand::List) return ListDirect(options, run_options);
  // A running host holds the store in memory and rewrites the whole file on
  // its next save, so a direct edit behind its back would be lost. The file is
  // only touched when the caller says the host is not running.
  if (!options.offline && run_options.prefer_pipe) return ResolveViaPipe(options, run_options);
  return ResolveDirect(options, run_options);
}

}  // namespace azookey::host
