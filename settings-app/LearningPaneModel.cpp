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

}  // namespace azookey::settings
