#include "LaunchArguments.h"

#include <Objbase.h>
#include <shellapi.h>

#include <memory>
#include <string>
#include <vector>

namespace {

bool IsOption(std::wstring_view value) {
  return value.size() >= 2 && value[0] == L'-' && value[1] == L'-';
}

struct LocalFreeDeleter {
  void operator()(LPWSTR* value) const noexcept {
    LocalFree(static_cast<HLOCAL>(static_cast<void*>(value)));
  }
};

bool ParseLangId(std::wstring_view value, LANGID* langid) {
  if (value.size() < 3 || value.size() > 6 || value[0] != L'0' ||
      (value[1] != L'x' && value[1] != L'X')) {
    return false;
  }

  unsigned int parsed = 0;
  for (const wchar_t character : value.substr(2)) {
    parsed <<= 4;
    if (character >= L'0' && character <= L'9') {
      parsed += static_cast<unsigned int>(character - L'0');
    } else if (character >= L'a' && character <= L'f') {
      parsed += static_cast<unsigned int>(character - L'a' + 10);
    } else if (character >= L'A' && character <= L'F') {
      parsed += static_cast<unsigned int>(character - L'A' + 10);
    } else {
      return false;
    }
  }

  *langid = static_cast<LANGID>(parsed);
  return true;
}

bool ParseProfile(std::wstring_view value, GUID* profile) {
  constexpr size_t kGuidLength = 38;
  if (value.size() != kGuidLength || value.front() != L'{' || value.back() != L'}' ||
      value[9] != L'-' || value[14] != L'-' || value[19] != L'-' || value[24] != L'-') {
    return false;
  }

  for (size_t index = 1; index + 1 < value.size(); ++index) {
    if (index == 9 || index == 14 || index == 19 || index == 24) continue;
    const wchar_t character = value[index];
    if (!((character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f') ||
          (character >= L'A' && character <= L'F'))) {
      return false;
    }
  }

  const std::wstring text(value);
  return IIDFromString(text.c_str(), profile) == S_OK;
}

azookey::settings::LaunchArguments ParseTokens(const std::vector<std::wstring_view>& arguments) {
  using azookey::settings::LaunchArgumentStatus;
  azookey::settings::LaunchArguments result;

  for (size_t index = 0; index < arguments.size();) {
    const std::wstring_view option = arguments[index];
    if (option != L"--langid" && option != L"--profile") {
      result.parse_error = true;
      ++index;
      continue;
    }

    const bool has_value = index + 1 < arguments.size() && !IsOption(arguments[index + 1]);
    if (option == L"--langid") {
      if (!has_value || result.langid_status != LaunchArgumentStatus::NotSupplied) {
        result.langid_status = LaunchArgumentStatus::Invalid;
      } else {
        result.langid_status = ParseLangId(arguments[index + 1], &result.langid)
                                   ? LaunchArgumentStatus::Valid
                                   : LaunchArgumentStatus::Invalid;
      }
    } else {
      if (!has_value || result.profile_status != LaunchArgumentStatus::NotSupplied) {
        result.profile_status = LaunchArgumentStatus::Invalid;
      } else {
        result.profile_status = ParseProfile(arguments[index + 1], &result.profile)
                                    ? LaunchArgumentStatus::Valid
                                    : LaunchArgumentStatus::Invalid;
      }
    }
    index += has_value ? 2 : 1;
  }

  return result;
}

}  // namespace

namespace azookey::settings {

LaunchArguments ParseLaunchArguments(std::wstring_view raw_arguments) noexcept {
  try {
    std::wstring command_line = L"azookey_settings.exe";
    if (!raw_arguments.empty()) {
      command_line.push_back(L' ');
      command_line.append(raw_arguments);
    }

    int argument_count = 0;
    LPWSTR* argument_values = CommandLineToArgvW(command_line.c_str(), &argument_count);
    if (argument_values == nullptr) {
      LaunchArguments result;
      result.parse_error = true;
      return result;
    }
    const std::unique_ptr<LPWSTR, LocalFreeDeleter> argument_owner(argument_values);

    std::vector<std::wstring_view> arguments;
    arguments.reserve(argument_count > 1 ? static_cast<size_t>(argument_count - 1) : 0);
    for (int index = 1; index < argument_count; ++index) {
      arguments.emplace_back(argument_values[index]);
    }
    return ParseTokens(arguments);
  } catch (...) {
    LaunchArguments result;
    result.parse_error = true;
    return result;
  }
}

}  // namespace azookey::settings
