#include "LearningPaneModel.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <utility>

namespace azookey::settings {

std::string LearningErrorResource(std::string_view error) {
  const bool known = std::find(kLearningErrorCodes.begin(), kLearningErrorCodes.end(), error) !=
                     kLearningErrorCodes.end();
  if (!known) return "LearningError_other";
  return "LearningError_" + std::string(error);
}

std::string FormatLearningDate(uint64_t epoch_seconds) {
  if (epoch_seconds == 0) return {};
  const auto time = static_cast<std::time_t>(epoch_seconds);
  std::tm local{};
  if (localtime_s(&local, &time) != 0) return {};
  char buffer[16]{};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local) == 0) return {};
  return buffer;
}

LearningTabKind LearningTabKindAt(size_t index) {
  if (index < kLearningStoreTabs.size()) return LearningTabKind::Store;
  return index == kLearningStoreTabs.size() ? LearningTabKind::Persona : LearningTabKind::Backup;
}

size_t LearningStoreTabIndex(std::string_view store) {
  for (size_t index = 0; index < kLearningStoreTabs.size(); ++index) {
    if (kLearningStoreTabs[index].id == store) return index;
  }
  return kLearningTabCount;
}

size_t LearningStoreTabToOpen(std::string_view store, bool busy) {
  return busy ? kLearningTabCount : LearningStoreTabIndex(store);
}

std::string JoinLearningTags(const std::vector<std::string>& tags) {
  std::string joined;
  for (const auto& tag : tags) {
    if (!joined.empty()) joined += ", ";
    joined += tag;
  }
  return joined;
}

LearningPageRange ComputeLearningPage(uint64_t offset, uint64_t shown, uint64_t total) {
  LearningPageRange range;
  if (shown > 0) {
    range.first = offset + 1;
    range.last = offset + shown;
  }
  range.has_previous = offset > 0;
  range.has_next = offset + shown < total;
  return range;
}

bool IsBackupArchivePath(std::string_view utf8_path) {
  if (utf8_path.size() < 5) return false;
  std::u8string text;
  text.reserve(utf8_path.size());
  for (const char ch : utf8_path) text.push_back(static_cast<char8_t>(ch));
  const std::filesystem::path path(text);
  if (!path.is_absolute() || !path.has_filename()) return false;
  auto extension = path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  return extension == L".zip";
}

namespace {

// UTF-8 of U+3000 IDEOGRAPHIC SPACE.
constexpr std::string_view kIdeographicSpace = "\xE3\x80\x80";

std::string_view TrimSpaces(std::string_view text) {
  for (;;) {
    if (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
      text.remove_prefix(1);
    } else if (text.starts_with(kIdeographicSpace)) {
      text.remove_prefix(kIdeographicSpace.size());
    } else {
      break;
    }
  }
  for (;;) {
    if (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
      text.remove_suffix(1);
    } else if (text.ends_with(kIdeographicSpace)) {
      text.remove_suffix(kIdeographicSpace.size());
    } else {
      break;
    }
  }
  return text;
}

// Katakana U+30A1..U+30F6 become hiragana U+3041..U+3096 (0x60 lower). Both ranges are three
// UTF-8 bytes starting E3 82 / E3 83, so the fold rewrites the last two bytes in place.
std::string FoldKatakanaToHiragana(std::string_view text) {
  std::string out(text);
  for (size_t i = 0; i + 2 < out.size(); ++i) {
    const auto b0 = static_cast<unsigned char>(out[i]);
    if (b0 != 0xE3) continue;
    const auto b1 = static_cast<unsigned char>(out[i + 1]);
    const auto b2 = static_cast<unsigned char>(out[i + 2]);
    if ((b1 != 0x82 && b1 != 0x83) || (b2 & 0xC0) != 0x80) continue;
    const char32_t code = 0x3000 | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
    if (code < 0x30A1 || code > 0x30F6) continue;
    const char32_t folded = code - 0x60;
    out[i + 1] = static_cast<char>(0x80 | ((folded >> 6) & 0x3F));
    out[i + 2] = static_cast<char>(0x80 | (folded & 0x3F));
    i += 2;
  }
  return out;
}

}  // namespace

UserWordInput NormalizeUserWordInput(std::string_view reading, std::string_view word) {
  UserWordInput input;
  input.reading = FoldKatakanaToHiragana(TrimSpaces(reading));
  input.word = std::string(TrimSpaces(word));
  if (input.reading.empty()) {
    input.error = UserWordInputError::EmptyReading;
  } else if (input.word.empty()) {
    input.error = UserWordInputError::EmptyWord;
  }
  return input;
}

std::string UserDictErrorResource(std::string_view error) {
  const bool known = std::find(kUserDictErrorCodes.begin(), kUserDictErrorCodes.end(), error) !=
                     kUserDictErrorCodes.end();
  if (!known) return "LearningUserDictError_other";
  return "LearningUserDictError_" + std::string(error);
}

UserDictHostFeatures UserDictHostFeaturesFrom(const std::vector<std::string>& capabilities) {
  const auto has = [&capabilities](std::string_view name) {
    return std::find(capabilities.begin(), capabilities.end(), name) != capabilities.end();
  };
  UserDictHostFeatures features;
  features.pos = has(azookey::ipc::kCapabilityUserDictPos);
  features.text_import = has(azookey::ipc::kCapabilityUserDictTextImport);
  return features;
}

std::string UserDictEntryPosResource(const azookey::ipc::LearningEntryField& entry) {
  if (const auto pos = entry.metadata.find("pos"); pos != entry.metadata.end()) {
    for (const auto& option : kUserDictPosOptions) {
      if (!option.id.empty() && option.id == pos->second) return std::string(option.resource);
    }
  }
  return std::string(kUserDictPosOptions.front().resource);
}

azookey::ipc::AddUserWordRequest MakeAddUserWordRequest(const UserWordInput& input,
                                                        size_t pos_index, bool host_has_pos) {
  azookey::ipc::AddUserWordRequest request;
  request.ruby = input.reading;
  request.word = input.word;
  if (host_has_pos && pos_index < kUserDictPosOptions.size() &&
      !kUserDictPosOptions[pos_index].id.empty()) {
    request.pos = std::string(kUserDictPosOptions[pos_index].id);
  }
  return request;
}

namespace {

template <size_t N>
bool Contains(const std::array<std::string_view, N>& values, std::string_view value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

std::string UserDictImportFormatResource(std::string_view format) {
  if (!Contains(kUserDictImportFormats, format)) return "UserDictImportFormat_other";
  return "UserDictImportFormat_" + std::string(format);
}

std::string UserDictImportEncodingResource(std::string_view encoding) {
  if (!Contains(kUserDictImportEncodings, encoding)) return "UserDictImportEncoding_other";
  std::string name(encoding);
  std::replace(name.begin(), name.end(), '-', '_');
  return "UserDictImportEncoding_" + name;
}

azookey::ipc::ImportUserDictionaryTextRequest MakeUserDictImportRequest(std::string source_path,
                                                                        size_t format_index,
                                                                        size_t conflict_index,
                                                                        bool dry_run) {
  azookey::ipc::ImportUserDictionaryTextRequest request;
  request.source_path = std::move(source_path);
  request.format = std::string(
      kUserDictImportFormats[format_index < kUserDictImportFormats.size() ? format_index : 0]);
  request.conflict_resolution = std::string(
      kUserDictImportConflicts[conflict_index < kUserDictImportConflicts.size() ? conflict_index
                                                                                : 0]);
  request.dry_run = dry_run;
  return request;
}

std::string UserDictImportErrorResource(std::string_view error) {
  if (!Contains(kUserDictImportErrorCodes, error)) return "UserDictImportError_other";
  return "UserDictImportError_" + std::string(error);
}

std::vector<UserDictImportCountLine> UserDictImportCountLines(
    const azookey::ipc::UserDictionaryTextImportCounts& counts, bool preview) {
  std::vector<UserDictImportCountLine> lines;
  lines.push_back({"UserDictImportCount_added", counts.added});
  const auto add = [&lines](const char* name, uint64_t count) {
    if (count > 0) lines.push_back({std::string("UserDictImportCount_") + name, count});
  };
  if (preview) {
    add("existing", counts.kept + counts.replaced);
  } else {
    add("kept", counts.kept);
    add("replaced", counts.replaced);
  }
  add("malformed", counts.malformed);
  add("too_long", counts.too_long);
  add("unsupported_pos", counts.unsupported_pos);
  add("pos_mismatch", counts.pos_mismatch);
  add("duplicates_in_file", counts.duplicates_in_file);
  return lines;
}

}  // namespace azookey::settings
