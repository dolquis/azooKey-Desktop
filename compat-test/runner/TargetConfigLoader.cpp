#include "runner/TargetConfigLoader.h"

#include <Windows.h>

#ifdef GetObject
#undef GetObject
#endif

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "azookey/ipc/Json.h"

namespace azookey::compat_test {
namespace {

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                      result.data(), size);
  return result;
}

std::filesystem::path StripSurroundingQuotes(const std::filesystem::path& path) {
  const std::wstring value = path.wstring();
  if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
    return std::filesystem::path(value.substr(1, value.size() - 2));
  }
  return path;
}

std::filesystem::path ExpandEnvironmentVariables(const std::filesystem::path& configured) {
  const DWORD required = ExpandEnvironmentStringsW(configured.c_str(), nullptr, 0);
  if (required == 0) return configured;
  std::vector<wchar_t> expanded(required);
  const DWORD length = ExpandEnvironmentStringsW(configured.c_str(), expanded.data(), required);
  if (length == 0 || length > required) return configured;
  return std::filesystem::path(expanded.data());
}

std::filesystem::path SearchPathExecutable(const std::filesystem::path& executable) {
  std::vector<wchar_t> searched(MAX_PATH);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const DWORD length = SearchPathW(nullptr, executable.c_str(), nullptr,
                                     static_cast<DWORD>(searched.size()), searched.data(), nullptr);
    if (length == 0) return {};
    if (length < searched.size())
      return std::filesystem::path(searched.data(), searched.data() + length);
    searched.resize(static_cast<size_t>(length) + 1);
  }
  return {};
}

std::filesystem::path AppPathExecutable(HKEY root, const std::wstring& executable,
                                        DWORD registry_view) {
  const std::wstring subkey =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + executable;
  constexpr DWORD kTypeFlags = RRF_RT_REG_SZ;
  for (int attempt = 0; attempt < 2; ++attempt) {
    DWORD value_bytes = 0;
    LSTATUS status = RegGetValueW(root, subkey.c_str(), nullptr, kTypeFlags | registry_view,
                                  nullptr, nullptr, &value_bytes);
    if (status != ERROR_SUCCESS || value_bytes < sizeof(wchar_t)) return {};
    std::vector<wchar_t> value(value_bytes / sizeof(wchar_t) + 1, L'\0');
    status = RegGetValueW(root, subkey.c_str(), nullptr, kTypeFlags | registry_view, nullptr,
                          value.data(), &value_bytes);
    if (status == ERROR_SUCCESS && value[0] != L'\0') {
      return StripSurroundingQuotes(std::filesystem::path(value.data()));
    }
    if (status != ERROR_MORE_DATA) return {};
  }
  return {};
}

}  // namespace

bool IsValidTargetId(std::string_view id) {
  return !id.empty() && std::all_of(id.begin(), id.end(), [](const unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
  });
}

std::optional<TargetConfig> LoadTargetConfig(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::nullopt;
  std::ostringstream contents;
  contents << stream.rdbuf();
  const auto parsed = azookey::ipc::json::Parse(contents.str());
  if (!parsed || !parsed->IsObject()) return std::nullopt;

  TargetConfig config;
  const auto id = parsed->GetString("id");
  const auto display_name = parsed->GetString("display_name");
  const auto app_id = parsed->GetString("app_id");
  const auto automation_level = parsed->GetString("automation_level");
  const auto* launch = parsed->GetObject("launch");
  const auto* window = parsed->GetObject("window");
  const auto* cases = parsed->GetArray("cases");
  if (!id || !IsValidTargetId(*id) || !display_name || !app_id || !automation_level || !launch ||
      !window || !cases) {
    return std::nullopt;
  }
  const azookey::ipc::json::Value launch_value(*launch);
  const azookey::ipc::json::Value window_value(*window);
  config.id = *id;
  config.display_name = *display_name;
  config.app_id = *app_id;
  config.automation_level = *automation_level;

  const auto executable = launch_value.GetString("executable");
  const auto window_class = window_value.GetString("class");
  const auto edit_class = window_value.GetString("edit_control_class");
  const auto editor_control_type = window_value.GetString("editor_control_type");
  const auto editor_name = window_value.GetString("editor_name");
  const auto candidate_class = window_value.GetString("candidate_window_class");
  if (!executable || !window_class || !edit_class || !candidate_class) return std::nullopt;
  config.executable = Utf8ToWide(*executable);
  config.window_classes.push_back(Utf8ToWide(*window_class));
  config.edit_control_class = Utf8ToWide(*edit_class);
  if (editor_control_type) config.editor_control_type = *editor_control_type;
  if (editor_name) config.editor_name = Utf8ToWide(*editor_name);
  config.candidate_window_class = Utf8ToWide(*candidate_class);

  if (const auto close_grace_period_ms = launch_value.GetUInt("close_grace_period_ms")) {
    if (*close_grace_period_ms == 0 || *close_grace_period_ms > 30000) return std::nullopt;
    config.close_grace_period_ms = static_cast<uint32_t>(*close_grace_period_ms);
  }
  if (const auto* args = launch_value.GetArray("args")) {
    for (const auto& arg : *args) {
      if (!arg.IsString()) return std::nullopt;
      config.arguments.push_back(Utf8ToWide(arg.AsString()));
    }
  }
  if (const auto* fallbacks = window_value.GetArray("class_fallbacks")) {
    for (const auto& item : *fallbacks) {
      if (!item.IsString()) return std::nullopt;
      config.window_classes.push_back(Utf8ToWide(item.AsString()));
    }
  }
  for (const auto& item : *cases) {
    if (!item.IsString()) return std::nullopt;
    config.cases.push_back(item.AsString());
  }
  if (const auto* workarounds = parsed->GetObject("workarounds")) {
    config.use_temporary_document =
        azookey::ipc::json::Value(*workarounds).GetBool("use_temporary_document").value_or(false);
  }
  if (const auto* temporary_document = launch_value.GetObject("temporary_document")) {
    const azookey::ipc::json::Value temporary_document_value(*temporary_document);
    const auto extension = temporary_document_value.GetString("extension");
    const auto document_contents = temporary_document_value.GetString("contents");
    if (!extension || !document_contents || extension->empty() || extension->front() != '.') {
      return std::nullopt;
    }
    config.use_temporary_document = true;
    config.temporary_document_extension = Utf8ToWide(*extension);
    config.temporary_document_contents = *document_contents;
    config.save_temporary_document_before_close =
        temporary_document_value.GetBool("save_before_close").value_or(true);
  }
  if (config.executable.empty() || config.window_classes.front().empty() ||
      config.edit_control_class.empty() || config.candidate_window_class.empty() ||
      config.cases.empty() ||
      (config.editor_control_type != "document" && config.editor_control_type != "edit")) {
    return std::nullopt;
  }
  return config;
}

ExecutableResolution ResolveExecutable(const std::filesystem::path& configured) {
  const auto unquoted = StripSurroundingQuotes(configured);
  const auto expanded = ExpandEnvironmentVariables(unquoted);
  if (expanded.has_parent_path()) {
    return {expanded, ExecutableResolutionSource::ConfiguredPath};
  }

  if (const auto searched = SearchPathExecutable(expanded); !searched.empty()) {
    return {searched, ExecutableResolutionSource::SearchPath};
  }

  for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    for (const DWORD view :
         {DWORD{0}, DWORD{RRF_SUBKEY_WOW6464KEY}, DWORD{RRF_SUBKEY_WOW6432KEY}}) {
      const auto app_path = AppPathExecutable(root, expanded.filename().wstring(), view);
      if (!app_path.empty()) return {app_path, ExecutableResolutionSource::AppPaths};
    }
  }
  return {expanded, ExecutableResolutionSource::Unresolved};
}

const char* ExecutableResolutionSourceName(ExecutableResolutionSource source) {
  switch (source) {
    case ExecutableResolutionSource::ConfiguredPath:
      return "configured-path";
    case ExecutableResolutionSource::SearchPath:
      return "search-path";
    case ExecutableResolutionSource::AppPaths:
      return "app-paths";
    case ExecutableResolutionSource::Unresolved:
      return "unresolved";
  }
  return "unresolved";
}

std::filesystem::path CreateTemporaryDocument(const TargetConfig& target) {
  wchar_t temp_directory[MAX_PATH]{};
  const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temp_directory)), temp_directory);
  if (length == 0 || length >= std::size(temp_directory)) return {};
  for (uint32_t attempt = 0; attempt < 100; ++attempt) {
    const auto path = std::filesystem::path(temp_directory) /
                      (L"azookey-compat-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt) +
                       target.temporary_document_extension);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      const bool write_succeeded =
          target.temporary_document_contents.empty() ||
          (target.temporary_document_contents.size() <= MAXDWORD &&
           WriteFile(file, target.temporary_document_contents.data(),
                     static_cast<DWORD>(target.temporary_document_contents.size()), &written,
                     nullptr) &&
           written == target.temporary_document_contents.size());
      CloseHandle(file);
      if (!write_succeeded) {
        DeleteFileW(path.c_str());
        return {};
      }
      return path;
    }
  }
  return {};
}

}  // namespace azookey::compat_test
