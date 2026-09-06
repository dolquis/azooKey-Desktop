#include "azookey/host/UserDictCli.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

#include "azookey/core/PlatformPaths.h"
#include "azookey/host/CliText.h"
#include "azookey/ipc/Json.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/learning/FileLock.h"
#include "azookey/learning/UserDictionary.h"

namespace azookey::host {

namespace {

namespace j = ::azookey::ipc::json;

constexpr uint64_t kHandshakeRequestId = 1;
constexpr uint64_t kCommandRequestId = 2;

bool SetError(std::string* error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
  return false;
}

bool ParseInt32(const std::string& value, int32_t* out) {
  char* end = nullptr;
  errno = 0;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0' ||
      parsed < std::numeric_limits<int32_t>::min() ||
      parsed > std::numeric_limits<int32_t>::max()) {
    return false;
  }
  *out = static_cast<int32_t>(parsed);
  return true;
}

bool ParseFiniteDouble(const std::string& value, double* out) {
  std::istringstream iss(value);
  iss.imbue(std::locale::classic());
  double parsed = 0.0;
  iss >> parsed;
  if (iss.fail() || !iss.eof() || !std::isfinite(parsed)) {
    return false;
  }
  *out = parsed;
  return true;
}

bool ReadOptionValue(const std::vector<std::string>& args, size_t* index, const char* flag,
                     std::string* value, std::string* error) {
  if (*index + 1 >= args.size()) {
    return SetError(error, std::string("missing value for ") + flag);
  }
  *value = args[++(*index)];
  return true;
}

j::Object BaseOperationJson(const char* op, bool ok) {
  j::Object object;
  object.emplace("op", j::Value(op));
  object.emplace("ok", j::Value(ok));
  return object;
}

void AddWordFields(j::Object* object, const learning::UserWord& word) {
  object->emplace("reading", j::Value(word.ruby));
  object->emplace("surface", j::Value(word.word));
  if (word.cid) {
    object->emplace("cid", j::Value(static_cast<int64_t>(*word.cid)));
  }
  if (word.mid) {
    object->emplace("mid", j::Value(static_cast<int64_t>(*word.mid)));
  }
  if (word.value) {
    object->emplace("weight", j::Value(*word.value));
  }
}

std::string OperationJsonLine(const char* op, bool ok, const learning::UserWord& word, bool dry_run,
                              const std::string& via, const std::string& error = {}) {
  auto object = BaseOperationJson(op, ok);
  AddWordFields(&object, word);
  if (dry_run) {
    object.emplace("dry_run", j::Value(true));
  }
  if (!via.empty()) {
    object.emplace("via", j::Value(via));
  }
  if (!error.empty()) {
    object.emplace("error", j::Value(error));
  }
  return j::Stringify(j::Value(std::move(object)));
}

std::string ListEmptyJsonLine() { return j::Stringify(j::Value(BaseOperationJson("list", true))); }

std::string ListEntryJsonLine(const learning::UserWord& word) {
  auto object = BaseOperationJson("list", true);
  AddWordFields(&object, word);
  return j::Stringify(j::Value(std::move(object)));
}

std::string ListEntryTsvLine(const learning::UserWord& word) {
  std::ostringstream oss;
  oss << SanitizeTsvCell(word.ruby) << '\t' << SanitizeTsvCell(word.word) << '\t';
  if (word.cid) oss << *word.cid;
  oss << '\t';
  if (word.mid) oss << *word.mid;
  oss << '\t';
  if (word.value) oss << *word.value;
  return oss.str();
}

std::string BulkOperationJsonLine(const char* op, bool ok, const std::string& path, size_t affected,
                                  size_t skipped, bool dry_run, const std::string& error = {}) {
  auto object = BaseOperationJson(op, ok);
  object.emplace("path", j::Value(path));
  object.emplace(op == std::string("import") ? "imported" : "exported",
                 j::Value(static_cast<uint64_t>(affected)));
  if (op == std::string("import")) {
    object.emplace("skipped", j::Value(static_cast<uint64_t>(skipped)));
  }
  object.emplace("via", j::Value("direct"));
  if (dry_run) {
    object.emplace("dry_run", j::Value(true));
  }
  if (!error.empty()) {
    object.emplace("error", j::Value(error));
  }
  return j::Stringify(j::Value(std::move(object)));
}

void StripTrailingCarriageReturn(std::string* value) {
  if (!value->empty() && value->back() == '\r') {
    value->pop_back();
  }
}

std::vector<std::string> SplitTsvLine(const std::string& line) {
  std::vector<std::string> cells;
  std::string cell;
  std::istringstream iss(line);
  while (std::getline(iss, cell, '\t')) {
    cells.push_back(cell);
  }
  if (!line.empty() && line.back() == '\t') {
    cells.emplace_back();
  }
  return cells;
}

bool ParseOptionalInt32Cell(const std::string& value, std::optional<int32_t>* out) {
  if (value.empty()) {
    out->reset();
    return true;
  }
  int32_t parsed = 0;
  if (!ParseInt32(value, &parsed)) {
    return false;
  }
  *out = parsed;
  return true;
}

bool ParseOptionalFiniteDoubleCell(const std::string& value, std::optional<double>* out) {
  if (value.empty()) {
    out->reset();
    return true;
  }
  double parsed = 0.0;
  if (!ParseFiniteDouble(value, &parsed)) {
    return false;
  }
  *out = parsed;
  return true;
}

std::optional<learning::UserWord> ParseUserWordTsvLine(std::string line) {
  StripTrailingCarriageReturn(&line);
  if (line.empty()) {
    return std::nullopt;
  }
  auto cells = SplitTsvLine(line);
  if (cells.size() < 2 || cells.size() > 5 || cells[0].empty() || cells[1].empty()) {
    return std::nullopt;
  }
  cells.resize(5);

  learning::UserWord word;
  word.ruby = cells[0];
  word.word = cells[1];
  if (!ParseOptionalInt32Cell(cells[2], &word.cid) ||
      !ParseOptionalInt32Cell(cells[3], &word.mid) ||
      !ParseOptionalFiniteDoubleCell(cells[4], &word.value)) {
    return std::nullopt;
  }
  return word;
}

learning::UserWord ToUserWord(const UserDictCliOptions& options) {
  learning::UserWord word;
  word.ruby = options.reading;
  word.word = options.surface;
  word.cid = options.cid;
  word.mid = options.mid;
  word.value = options.weight;
  return word;
}

bool ContainsWord(const std::vector<learning::UserWord>& entries, const learning::UserWord& word) {
  return std::any_of(entries.begin(), entries.end(), [&](const learning::UserWord& entry) {
    return entry.ruby == word.ruby && entry.word == word.word;
  });
}

void SortEntries(std::vector<learning::UserWord>* entries) {
  std::sort(entries->begin(), entries->end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.ruby != rhs.ruby) return lhs.ruby < rhs.ruby;
    return lhs.word < rhs.word;
  });
}

azookey::ipc::Envelope MakeEnvelope(uint64_t request_id, azookey::ipc::MessageType type,
                                    std::string payload_json) {
  azookey::ipc::Envelope env;
  env.request_id = request_id;
  env.trace_id = "userdict-cli";
  env.type = type;
  env.payload_json = std::move(payload_json);
  return env;
}

UserDictCliResult RunViaPipe(const UserDictCliOptions& options,
                             const UserDictCliRunOptions& run_options) {
  UserDictCliResult result;
  const auto word = ToUserWord(options);
  const char* op = options.command == UserDictCliCommand::Add ? "add" : "remove";
  azookey::ipc::NamedPipeClient client;
  const std::string pipe_name =
      run_options.pipe_name.empty() ? azookey::ipc::DefaultPipeName() : run_options.pipe_name;
  if (!client.Connect(pipe_name, run_options.connect_timeout_ms)) {
    result.exit_code = 1;
    result.error = "failed to connect to running host; pass --offline to edit the file directly";
    result.output_lines.push_back(OperationJsonLine(op, false, word, false, "ipc", result.error));
    return result;
  }

  azookey::ipc::HandshakeRequest handshake;
  handshake.tip_version = "userdict-cli";
  handshake.protocol_version = 1;
  handshake.capabilities = {"userdict-cli"};
  handshake.handshake_token = run_options.handshake_token;
  if (!client.Send(MakeEnvelope(kHandshakeRequestId, azookey::ipc::MessageType::Handshake,
                                azookey::ipc::BuildHandshakeRequest(handshake)))) {
    result.exit_code = 1;
    result.error = "failed to send handshake to running host";
    return result;
  }
  auto handshake_resp = client.ReceiveWithTimeout(run_options.response_timeout_ms);
  if (!handshake_resp) {
    result.exit_code = 1;
    result.error = "timed out waiting for handshake response from running host";
    return result;
  }
  auto handshake_payload = azookey::ipc::ParseHandshakeResponse(handshake_resp->payload_json);
  if (!handshake_payload || !handshake_payload->accepted) {
    result.exit_code = 1;
    result.error = "handshake rejected by running host";
    return result;
  }

  azookey::ipc::Envelope command;
  if (options.command == UserDictCliCommand::Add) {
    azookey::ipc::AddUserWordRequest req;
    req.ruby = word.ruby;
    req.word = word.word;
    req.cid = word.cid;
    req.mid = word.mid;
    req.value = word.value;
    command = MakeEnvelope(kCommandRequestId, azookey::ipc::MessageType::AddUserWord,
                           azookey::ipc::BuildAddUserWordRequest(req));
  } else {
    azookey::ipc::RemoveUserWordRequest req;
    req.ruby = word.ruby;
    req.word = word.word;
    command = MakeEnvelope(kCommandRequestId, azookey::ipc::MessageType::RemoveUserWord,
                           azookey::ipc::BuildRemoveUserWordRequest(req));
  }

  if (!client.Send(command)) {
    result.exit_code = 1;
    result.error = "failed to send userdict command to running host";
    return result;
  }
  auto command_resp = client.ReceiveWithTimeout(run_options.response_timeout_ms);
  if (!command_resp) {
    result.exit_code = 1;
    result.error = "timed out waiting for userdict response from running host";
    return result;
  }

  bool ok = false;
  if (options.command == UserDictCliCommand::Add) {
    if (auto parsed = azookey::ipc::ParseAddUserWordResponse(command_resp->payload_json)) {
      ok = parsed->ok;
    }
  } else if (auto parsed = azookey::ipc::ParseRemoveUserWordResponse(command_resp->payload_json)) {
    ok = parsed->ok;
  }

  const std::string error = ok ? std::string() : std::string("running host rejected ") + op;
  result.exit_code = ok ? 0 : 1;
  result.output_lines.push_back(OperationJsonLine(op, ok, word, false, "ipc", error));
  if (!ok) {
    result.error = error;
  }
  return result;
}

UserDictCliResult RunDirect(const UserDictCliOptions& options,
                            const UserDictCliRunOptions& run_options) {
  UserDictCliResult result;
  auto file_lock = azookey::learning::AcquireExclusiveFileLockForPath(run_options.user_dict_path);
  if (!file_lock) {
    result.exit_code = 1;
    result.error = "failed to lock user dictionary";
    return result;
  }
  azookey::learning::UserDictionary dict(run_options.user_dict_path);
  if (!dict.Load()) {
    result.exit_code = 1;
    result.error = "failed to load user dictionary";
    return result;
  }

  if (options.command == UserDictCliCommand::List) {
    // list intentionally reads the persisted file. The IPC add/remove path saves
    // synchronously, so the file is the shared source for CLI inspection.
    auto entries = dict.All();
    SortEntries(&entries);
    if (options.format == UserDictCliFormat::Tsv) {
      for (const auto& entry : entries) {
        result.output_lines.push_back(ListEntryTsvLine(entry));
      }
    } else if (entries.empty()) {
      result.output_lines.push_back(ListEmptyJsonLine());
    } else {
      for (const auto& entry : entries) {
        result.output_lines.push_back(ListEntryJsonLine(entry));
      }
    }
    return result;
  }

  if (options.command == UserDictCliCommand::Import) {
    // options.path holds UTF-8 bytes so it can be echoed back in the JSON
    // report; Utf8Path is what turns it into a path without going through the
    // Windows active code page.
    std::ifstream input(azookey::core::Utf8Path(options.path));
    if (!input.is_open()) {
      result.exit_code = 1;
      result.error = "failed to open import TSV";
      result.output_lines.push_back(BulkOperationJsonLine("import", false, options.path, 0, 0,
                                                          options.dry_run, result.error));
      return result;
    }

    const auto before = dict.All();
    std::string line;
    size_t imported = 0;
    size_t skipped = 0;
    while (std::getline(input, line)) {
      StripTrailingCarriageReturn(&line);
      if (line.empty()) {
        continue;
      }
      if (auto word = ParseUserWordTsvLine(line)) {
        if (!options.dry_run) {
          dict.Add(*word);
        }
        ++imported;
      } else {
        ++skipped;
      }
    }

    if (!options.dry_run && !dict.Save()) {
      dict.ReplaceAll(before);
      result.exit_code = 1;
      result.error = "failed to save user dictionary";
    }
    result.output_lines.push_back(BulkOperationJsonLine("import", result.exit_code == 0,
                                                        options.path, imported, skipped,
                                                        options.dry_run, result.error));
    return result;
  }

  if (options.command == UserDictCliCommand::Export) {
    auto entries = dict.All();
    SortEntries(&entries);
    if (!options.dry_run) {
      azookey::learning::UserDictionary exported(azookey::core::Utf8Path(options.path));
      exported.ReplaceAll(entries);
      if (!exported.Save()) {
        result.exit_code = 1;
        result.error = "failed to save export JSON";
      }
    }
    result.output_lines.push_back(BulkOperationJsonLine("export", result.exit_code == 0,
                                                        options.path, entries.size(), 0,
                                                        options.dry_run, result.error));
    return result;
  }

  const auto word = ToUserWord(options);
  if (options.command == UserDictCliCommand::Add) {
    if (!options.dry_run) {
      dict.Add(word);
      if (!dict.Save()) {
        result.exit_code = 1;
        result.error = "failed to save user dictionary";
      }
    }
    result.output_lines.push_back(OperationJsonLine("add", result.exit_code == 0, word,
                                                    options.dry_run, "direct", result.error));
    return result;
  }

  const bool exists = ContainsWord(dict.All(), word);
  if (!options.dry_run) {
    if (!dict.Remove(word.word, word.ruby)) {
      result.exit_code = 1;
      result.error = "entry not found";
    } else if (!dict.Save()) {
      result.exit_code = 1;
      result.error = "failed to save user dictionary";
    }
  } else if (!exists) {
    result.exit_code = 1;
    result.error = "entry not found";
  }
  result.output_lines.push_back(OperationJsonLine("remove", result.exit_code == 0, word,
                                                  options.dry_run, "direct", result.error));
  return result;
}

}  // namespace

std::optional<UserDictCliOptions> ParseUserDictCliArgs(const std::vector<std::string>& args,
                                                       std::string* error) {
  if (args.empty()) {
    SetError(error, "missing userdict subcommand");
    return std::nullopt;
  }

  UserDictCliOptions options;
  const std::string& command = args[0];
  if (command == "add") {
    options.command = UserDictCliCommand::Add;
  } else if (command == "remove") {
    options.command = UserDictCliCommand::Remove;
  } else if (command == "list") {
    options.command = UserDictCliCommand::List;
  } else if (command == "import") {
    options.command = UserDictCliCommand::Import;
  } else if (command == "export") {
    options.command = UserDictCliCommand::Export;
  } else {
    SetError(error, "unknown userdict subcommand: " + command);
    return std::nullopt;
  }

  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--dry-run") {
      options.dry_run = true;
    } else if (arg == "--offline") {
      options.offline = true;
    } else if (arg == "--reading") {
      if (!ReadOptionValue(args, &i, "--reading", &options.reading, error)) return std::nullopt;
    } else if (arg == "--surface") {
      if (!ReadOptionValue(args, &i, "--surface", &options.surface, error)) return std::nullopt;
    } else if (arg == "--cid") {
      std::string value;
      if (!ReadOptionValue(args, &i, "--cid", &value, error)) return std::nullopt;
      int32_t parsed = 0;
      if (!ParseInt32(value, &parsed)) {
        SetError(error, "invalid --cid value: " + value);
        return std::nullopt;
      }
      options.cid = parsed;
    } else if (arg == "--mid") {
      std::string value;
      if (!ReadOptionValue(args, &i, "--mid", &value, error)) return std::nullopt;
      int32_t parsed = 0;
      if (!ParseInt32(value, &parsed)) {
        SetError(error, "invalid --mid value: " + value);
        return std::nullopt;
      }
      options.mid = parsed;
    } else if (arg == "--weight") {
      std::string value;
      if (!ReadOptionValue(args, &i, "--weight", &value, error)) return std::nullopt;
      double parsed = 0.0;
      if (!ParseFiniteDouble(value, &parsed)) {
        SetError(error, "invalid --weight value: " + value);
        return std::nullopt;
      }
      options.weight = parsed;
    } else if (arg == "--format") {
      std::string value;
      if (!ReadOptionValue(args, &i, "--format", &value, error)) return std::nullopt;
      if (value == "json") {
        options.format = UserDictCliFormat::Json;
      } else if (value == "tsv") {
        options.format = UserDictCliFormat::Tsv;
      } else {
        SetError(error, "invalid --format value: " + value);
        return std::nullopt;
      }
    } else if ((options.command == UserDictCliCommand::Import ||
                options.command == UserDictCliCommand::Export) &&
               options.path.empty()) {
      options.path = arg;
    } else {
      SetError(error, "unknown userdict option: " + arg);
      return std::nullopt;
    }
  }

  if (options.command == UserDictCliCommand::Add || options.command == UserDictCliCommand::Remove) {
    if (options.reading.empty()) {
      SetError(error, "--reading is required");
      return std::nullopt;
    }
    if (options.surface.empty()) {
      SetError(error, "--surface is required");
      return std::nullopt;
    }
  }
  if (options.command == UserDictCliCommand::Import ||
      options.command == UserDictCliCommand::Export) {
    if (options.path.empty()) {
      SetError(error, command + " path is required");
      return std::nullopt;
    }
  }

  return options;
}

UserDictCliResult RunUserDictCli(const UserDictCliOptions& options,
                                 const UserDictCliRunOptions& run_options) {
  if (!options.offline && run_options.prefer_pipe && !options.dry_run &&
      (options.command == UserDictCliCommand::Add ||
       options.command == UserDictCliCommand::Remove)) {
    return RunViaPipe(options, run_options);
  }
  return RunDirect(options, run_options);
}

}  // namespace azookey::host
