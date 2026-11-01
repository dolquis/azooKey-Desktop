#include "LearningPaneModel.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <filesystem>

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

std::string FormatLearningWeight(double weight) {
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "%.1f", weight);
  return buffer;
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

}  // namespace azookey::settings
