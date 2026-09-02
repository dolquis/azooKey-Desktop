#include "azookey/host/LookupCli.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azookey/ipc/Json.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/UserDictionary.h"

namespace azookey::host {
namespace {

namespace j = ::azookey::ipc::json;

enum class LookupSource {
  UserDictionary,
  Learning,
};

struct LookupMatch {
  LookupSource source{LookupSource::UserDictionary};
  std::string reading;
  std::string surface;
  std::optional<int32_t> cid;
  std::optional<int32_t> mid;
  std::optional<double> weight;
  std::optional<uint64_t> last_updated_epoch_sec;
};

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

const char* ModeName(LookupCliMode mode) {
  switch (mode) {
    case LookupCliMode::ExactReading:
      return "exact";
    case LookupCliMode::ReadingPrefix:
      return "prefix";
    case LookupCliMode::Surface:
      return "surface";
  }
  return "exact";
}

const char* SourceName(LookupSource source) {
  return source == LookupSource::UserDictionary ? "user_dict" : "learning";
}

bool Matches(const LookupCliOptions& options, const std::string& reading,
             const std::string& surface) {
  switch (options.mode) {
    case LookupCliMode::ExactReading:
      return reading == options.query;
    case LookupCliMode::ReadingPrefix:
      return reading.rfind(options.query, 0) == 0;
    case LookupCliMode::Surface:
      return surface == options.query;
  }
  return false;
}

std::string SanitizeTsvCell(std::string value) {
  for (char& ch : value) {
    if (ch == '\t' || ch == '\r' || ch == '\n') ch = ' ';
  }
  return value;
}

std::string JsonLine(const LookupCliOptions& options, const LookupMatch& match) {
  j::Object object;
  object.emplace("op", j::Value("lookup"));
  object.emplace("ok", j::Value(true));
  object.emplace("mode", j::Value(ModeName(options.mode)));
  object.emplace("query", j::Value(options.query));
  object.emplace("source", j::Value(SourceName(match.source)));
  object.emplace("reading", j::Value(match.reading));
  object.emplace("surface", j::Value(match.surface));
  if (match.cid) object.emplace("cid", j::Value(static_cast<int64_t>(*match.cid)));
  if (match.mid) object.emplace("mid", j::Value(static_cast<int64_t>(*match.mid)));
  if (match.weight) object.emplace("weight", j::Value(*match.weight));
  if (match.last_updated_epoch_sec) {
    object.emplace("last_updated_epoch_sec", j::Value(*match.last_updated_epoch_sec));
  }
  return j::Stringify(j::Value(std::move(object)));
}

std::string EmptyJsonLine(const LookupCliOptions& options) {
  j::Object object;
  object.emplace("op", j::Value("lookup"));
  object.emplace("ok", j::Value(true));
  object.emplace("mode", j::Value(ModeName(options.mode)));
  object.emplace("query", j::Value(options.query));
  object.emplace("count", j::Value(uint64_t{0}));
  return j::Stringify(j::Value(std::move(object)));
}

std::string TsvLine(const LookupMatch& match) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << SourceName(match.source) << '\t' << SanitizeTsvCell(match.reading) << '\t'
      << SanitizeTsvCell(match.surface) << '\t';
  if (match.cid) out << *match.cid;
  out << '\t';
  if (match.mid) out << *match.mid;
  out << '\t';
  if (match.weight) out << *match.weight;
  out << '\t';
  if (match.last_updated_epoch_sec) out << *match.last_updated_epoch_sec;
  return out.str();
}

bool PathExists(const std::filesystem::path& path, bool* exists) {
  std::error_code error;
  *exists = std::filesystem::exists(path, error);
  return !error;
}

}  // namespace

std::optional<LookupCliOptions> ParseLookupCliArgs(const std::vector<std::string>& args,
                                                   std::string* error) {
  LookupCliOptions options;
  bool has_mode = false;
  bool has_query = false;
  for (size_t i = 0; i < args.size(); ++i) {
    const auto& arg = args[i];
    if (arg == "--mode") {
      std::string value;
      if (!ReadOptionValue(args, &i, "--mode", &value, error)) return std::nullopt;
      if (value == "exact") {
        options.mode = LookupCliMode::ExactReading;
      } else if (value == "prefix") {
        options.mode = LookupCliMode::ReadingPrefix;
      } else if (value == "surface") {
        options.mode = LookupCliMode::Surface;
      } else {
        SetError(error, "invalid --mode value: " + value);
        return std::nullopt;
      }
      has_mode = true;
    } else if (arg == "--query") {
      if (!ReadOptionValue(args, &i, "--query", &options.query, error)) return std::nullopt;
      has_query = true;
    } else if (arg == "--format") {
      std::string value;
      if (!ReadOptionValue(args, &i, "--format", &value, error)) return std::nullopt;
      if (value == "json") {
        options.format = LookupCliFormat::Json;
      } else if (value == "tsv") {
        options.format = LookupCliFormat::Tsv;
      } else {
        SetError(error, "invalid --format value: " + value);
        return std::nullopt;
      }
    } else {
      SetError(error, "unknown lookup option: " + arg);
      return std::nullopt;
    }
  }
  if (!has_mode) {
    SetError(error, "--mode is required");
    return std::nullopt;
  }
  if (!has_query || options.query.empty()) {
    SetError(error, "--query is required");
    return std::nullopt;
  }
  return options;
}

LookupCliResult RunLookupCli(const LookupCliOptions& options,
                             const LookupCliRunOptions& run_options) {
  LookupCliResult result;
  std::vector<LookupMatch> matches;

  bool user_dict_exists = false;
  if (!PathExists(run_options.user_dict_path, &user_dict_exists)) {
    result.exit_code = 1;
    result.error = "failed to inspect user dictionary path";
    return result;
  }
  if (user_dict_exists) {
    learning::UserDictionary dictionary(run_options.user_dict_path.string());
    if (!dictionary.LoadReadOnly()) {
      result.exit_code = 1;
      result.error = "failed to load user dictionary";
      return result;
    }
    for (const auto& word : dictionary.All()) {
      if (!Matches(options, word.ruby, word.word)) continue;
      matches.push_back({LookupSource::UserDictionary, word.ruby, word.word, word.cid, word.mid,
                         word.value, std::nullopt});
    }
  }

  bool learning_exists = false;
  if (!PathExists(run_options.learning_path, &learning_exists)) {
    result.exit_code = 1;
    result.error = "failed to inspect learning store path";
    return result;
  }
  if (learning_exists) {
    learning::LearningStore store(run_options.learning_path.string());
    if (!store.Load()) {
      result.exit_code = 1;
      result.error = "failed to load learning store";
      return result;
    }
    for (const auto& entry : store.All()) {
      if (!Matches(options, entry.reading, entry.surface)) continue;
      matches.push_back({LookupSource::Learning, entry.reading, entry.surface, std::nullopt,
                         std::nullopt, entry.record.weight, entry.record.last_updated_epoch_sec});
    }
  }

  std::sort(matches.begin(), matches.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.reading != rhs.reading) return lhs.reading < rhs.reading;
    if (lhs.surface != rhs.surface) return lhs.surface < rhs.surface;
    return std::string_view(SourceName(lhs.source)) < std::string_view(SourceName(rhs.source));
  });

  if (options.format == LookupCliFormat::Json) {
    if (matches.empty()) {
      result.output_lines.push_back(EmptyJsonLine(options));
    } else {
      for (const auto& match : matches) result.output_lines.push_back(JsonLine(options, match));
    }
  } else {
    for (const auto& match : matches) result.output_lines.push_back(TsvLine(match));
  }
  return result;
}

}  // namespace azookey::host
