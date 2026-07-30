#include "Diagnostics.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: Windows.h must precede headers that depend on Win32 types.
#include <Windows.h>
#include <TlHelp32.h>
#include <msctf.h>
#include <winternl.h>
// clang-format on
#ifdef GetObject
#undef GetObject
#endif
#endif

#include "SettingsSchema.h"
#include "azookey/core/Redaction.h"
#include "azookey/host/UserDataPaths.h"
#include "azookey/ipc/Json.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/tsf/TextServiceFactory.h"

namespace azookey::diagnostics {

namespace {

namespace j = ::azookey::ipc::json;

constexpr uint32_t kHandshakeTimeoutMs = 3000;
constexpr uint32_t kConnectTimeoutMs = 500;
constexpr uint32_t kPingTimeoutMs = 500;
constexpr uint32_t kDiagnosticsTimeoutMs = 1000;

uint64_t NowMs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

Status Worst(Status left, Status right) {
  return static_cast<int>(left) >= static_cast<int>(right) ? left : right;
}

std::string JsonObject(j::Object object) { return j::Stringify(j::Value(std::move(object))); }

void AddCheck(Report& report, std::string id, std::string name, Status status, std::string message,
              j::Object details = {}) {
  report.status = Worst(report.status, status);
  report.checks.push_back(
      {std::move(id), std::move(name), status, std::move(message), JsonObject(std::move(details))});
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (!input.good() && !input.eof()) return std::nullopt;
  return buffer.str();
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::string EnvironmentValue(const char* name) {
#ifdef _WIN32
  char* buffer = nullptr;
  size_t size = 0;
  if (_dupenv_s(&buffer, &size, name) != 0 || !buffer) return {};
  std::string value(buffer);
  std::free(buffer);
  return value;
#else
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
#endif
}

bool IsSensitiveKey(std::string_view key) {
  const auto lower = LowerAscii(std::string(key));
  return lower.find("apikey") != std::string::npos || lower.find("api_key") != std::string::npos ||
         lower.find("token") != std::string::npos || lower.find("secret") != std::string::npos ||
         lower.find("prompt") != std::string::npos;
}

j::Value RedactValue(const j::Value& value, std::string_view key = {}) {
  if (!key.empty() && IsSensitiveKey(key)) return j::Value("***redacted***");
  if (value.IsObject()) {
    j::Object redacted;
    for (const auto& [child_key, child] : value.AsObject()) {
      redacted.emplace(child_key, RedactValue(child, child_key));
    }
    return j::Value(std::move(redacted));
  }
  if (value.IsArray()) {
    j::Array redacted;
    redacted.reserve(value.AsArray().size());
    for (const auto& child : value.AsArray()) redacted.emplace_back(RedactValue(child));
    return j::Value(std::move(redacted));
  }
  return value;
}

bool JsonValueEquals(const j::Value& left, const j::Value& right) {
  if (left.data.index() != right.data.index()) return false;
  if (left.IsNull()) return true;
  if (left.IsBool()) return left.AsBool() == right.AsBool();
  if (left.IsNumber()) return left.AsNumber() == right.AsNumber();
  if (left.IsString()) return left.AsString() == right.AsString();
  return j::Stringify(left) == j::Stringify(right);
}

bool MatchesType(const j::Value& value, std::string_view type) {
  if (type == "object") return value.IsObject();
  if (type == "array") return value.IsArray();
  if (type == "string") return value.IsString();
  if (type == "boolean") return value.IsBool();
  if (type == "number") return value.IsNumber();
  if (type == "integer") {
    if (!value.IsNumber()) return false;
    const double number = value.AsNumber();
    return number >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
           number <= static_cast<double>(std::numeric_limits<int64_t>::max()) &&
           number == static_cast<double>(static_cast<int64_t>(number));
  }
  if (type == "null") return value.IsNull();
  return true;
}

bool ValidateJsonSchema(const j::Value& value, const j::Value& schema) {
  if (!schema.IsObject()) return true;
  if (const auto type = schema.GetString("type"); type && !MatchesType(value, *type)) return false;
  if (const auto* values = schema.GetArray("enum")) {
    const bool matched = std::any_of(values->begin(), values->end(), [&](const auto& candidate) {
      return JsonValueEquals(value, candidate);
    });
    if (!matched) return false;
  }
  if (value.IsNumber()) {
    if (const auto minimum = schema.GetNumber("minimum"); minimum && value.AsNumber() < *minimum) {
      return false;
    }
  }
  if (value.IsObject()) {
    const auto* properties = schema.GetObject("properties");
    const bool allow_additional = !schema.Find("additionalProperties") ||
                                  !schema.Find("additionalProperties")->IsBool() ||
                                  schema.Find("additionalProperties")->AsBool();
    for (const auto& [key, child] : value.AsObject()) {
      const j::Value* child_schema = nullptr;
      if (properties) {
        const auto found = properties->find(key);
        if (found != properties->end()) child_schema = &found->second;
      }
      if (!child_schema) {
        if (!allow_additional) return false;
        continue;
      }
      if (!ValidateJsonSchema(child, *child_schema)) return false;
    }
  }
  if (value.IsArray()) {
    if (const auto* item_schema = schema.Find("items")) {
      for (const auto& child : value.AsArray()) {
        if (!ValidateJsonSchema(child, *item_schema)) return false;
      }
    }
  }
  return true;
}

bool SchemaUsesOnlySupportedKeywords(const j::Value& schema) {
  if (!schema.IsObject()) return true;
  constexpr std::array<std::string_view, 10> supported = {
      "$schema", "additionalProperties", "default", "description", "enum", "items",
      "minimum", "properties",           "title",   "type",
  };
  for (const auto& [key, value] : schema.AsObject()) {
    if (std::find(supported.begin(), supported.end(), key) == supported.end()) return false;
    if (key == "properties") {
      if (!value.IsObject()) return false;
      for (const auto& [_, child_schema] : value.AsObject()) {
        if (!SchemaUsesOnlySupportedKeywords(child_schema)) return false;
      }
    } else if (key == "items" && !SchemaUsesOnlySupportedKeywords(value)) {
      return false;
    }
  }
  return true;
}

struct SettingsProbe {
  bool valid{true};
  bool missing{true};
  bool model_enabled{true};
  std::string selected_path;
};

SettingsProbe ProbeSettings(const std::filesystem::path& path) {
  SettingsProbe result;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return result;
  result.missing = false;
  const auto text = ReadTextFile(path);
  if (!text) {
    result.valid = false;
    return result;
  }
  const auto value = j::Parse(*text);
  const auto schema = j::Parse(kSettingsSchemaJson);
  if (!value || !value->IsObject() || !schema || !ValidateJsonSchema(*value, *schema)) {
    result.valid = false;
    return result;
  }
  if (const auto* model = value->GetObject("model")) {
    const auto enabled = model->find("enabled");
    if (enabled != model->end() && enabled->second.IsBool()) {
      result.model_enabled = enabled->second.AsBool();
    }
    const auto selected = model->find("selectedPath");
    if (selected != model->end() && selected->second.IsString()) {
      result.selected_path = selected->second.AsString();
    }
  }
  return result;
}

std::filesystem::path ExpandEnvironmentPath(const std::string& path) {
#ifdef _WIN32
  if (path.empty()) return {};
  const int wide_size =
      MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
  if (wide_size <= 0) return std::filesystem::path(path);
  std::wstring wide(static_cast<size_t>(wide_size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wide.data(),
                      wide_size);
  const DWORD needed = ExpandEnvironmentStringsW(wide.c_str(), nullptr, 0);
  if (needed == 0) return std::filesystem::path(wide);
  std::wstring expanded(needed, L'\0');
  if (ExpandEnvironmentStringsW(wide.c_str(), expanded.data(), needed) == 0) {
    return std::filesystem::path(wide);
  }
  if (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
  return std::filesystem::path(expanded);
#else
  return std::filesystem::path(path);
#endif
}

bool PathsEquivalent(const std::string& left, const std::string& right) {
  if (left.empty() || right.empty()) return false;
  const auto normalized_left = ExpandEnvironmentPath(left).lexically_normal().string();
  const auto normalized_right = ExpandEnvironmentPath(right).lexically_normal().string();
#ifdef _WIN32
  return LowerAscii(normalized_left) == LowerAscii(normalized_right);
#else
  return normalized_left == normalized_right;
#endif
}

bool ValidateGguf(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::array<unsigned char, 8> header{};
  if (!input.read(reinterpret_cast<char*>(header.data()), header.size())) return false;
  if (header[0] != 'G' || header[1] != 'G' || header[2] != 'U' || header[3] != 'F') return false;
  const uint32_t version =
      static_cast<uint32_t>(header[4]) | (static_cast<uint32_t>(header[5]) << 8) |
      (static_cast<uint32_t>(header[6]) << 16) | (static_cast<uint32_t>(header[7]) << 24);
  return version >= 1 && version <= 3;
}

void CollectOnnxReferences(const j::Value& value, std::vector<std::filesystem::path>& paths) {
  if (value.IsString()) {
    const std::filesystem::path path(value.AsString());
    if (LowerAscii(path.extension().string()) == ".onnx") paths.push_back(path);
    return;
  }
  if (value.IsArray()) {
    for (const auto& child : value.AsArray()) CollectOnnxReferences(child, paths);
    return;
  }
  if (value.IsObject()) {
    for (const auto& [_, child] : value.AsObject()) CollectOnnxReferences(child, paths);
  }
}

bool ValidateOnnxGenAi(const std::filesystem::path& directory) {
  const auto config_path = directory / "genai_config.json";
  const auto text = ReadTextFile(config_path);
  const auto config = text ? j::Parse(*text) : std::nullopt;
  if (!config || !config->IsObject()) return false;
  std::vector<std::filesystem::path> references;
  CollectOnnxReferences(*config, references);
  if (references.empty()) return false;
  for (const auto& reference : references) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(directory / reference, ec) || ec) return false;
  }
  return true;
}

bool ValidateModel(const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::is_regular_file(path, ec) && !ec) {
    return LowerAscii(path.extension().string()) == ".gguf" && ValidateGguf(path);
  }
  ec.clear();
  return std::filesystem::is_directory(path, ec) && !ec && ValidateOnnxGenAi(path);
}

bool ParseLearningRecord(std::string_view line) {
  if (line.empty() || line == learning::kLearningStoreEscapedTsvHeader) return true;
  const auto first = line.find('\t');
  const auto second = first == std::string_view::npos ? first : line.find('\t', first + 1);
  if (first == std::string_view::npos || second == std::string_view::npos) return false;
  const auto values = line.substr(second + 1);
  const auto separator = values.find(' ');
  if (separator == std::string_view::npos ||
      values.find(' ', separator + 1) != std::string_view::npos) {
    return false;
  }
  double weight = 0.0;
  const auto weight_result = std::from_chars(values.data(), values.data() + separator, weight);
  if (weight_result.ec != std::errc{} || weight_result.ptr != values.data() + separator ||
      !std::isfinite(weight) || weight < 0.0) {
    return false;
  }
  uint64_t timestamp = 0;
  const auto time_begin = values.data() + separator + 1;
  const auto time_result = std::from_chars(time_begin, values.data() + values.size(), timestamp);
  return time_result.ec == std::errc{} && time_result.ptr == values.data() + values.size();
}

bool ProbeLearningStore(const std::filesystem::path& path, uint64_t* entries) {
  *entries = 0;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return true;
  std::ifstream input(path);
  if (!input) return false;
  std::string line;
  while (std::getline(input, line)) {
    if (!ParseLearningRecord(line)) return false;
    if (!line.empty() && line != learning::kLearningStoreEscapedTsvHeader) ++*entries;
  }
  return input.eof();
}

bool ProbeUserDictionary(const std::filesystem::path& path, uint64_t* entries,
                         uint64_t* skipped_entries) {
  *entries = 0;
  *skipped_entries = 0;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return true;
  const auto text = ReadTextFile(path);
  const auto value = text ? j::Parse(*text) : std::nullopt;
  if (!value || !value->IsObject()) return false;
  const auto* words = value->GetArray("entries");
  if (!words) return false;
  for (const auto& word : *words) {
    if (!word.IsObject() || !word.GetString("word") || !word.GetString("ruby")) {
      ++*skipped_entries;
      continue;
    }
    ++*entries;
  }
  return true;
}

#ifdef _WIN32
std::wstring GuidString(REFGUID guid) {
  std::array<wchar_t, 40> buffer{};
  const int length = StringFromGUID2(guid, buffer.data(), static_cast<int>(buffer.size()));
  return length > 0 ? std::wstring(buffer.data(), static_cast<size_t>(length - 1)) : std::wstring();
}

std::string WideToUtf8(std::wstring_view value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

std::optional<std::wstring> ReadRegistryString(HKEY root, const std::wstring& subkey,
                                               const wchar_t* value_name) {
  HKEY raw_key = nullptr;
  if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &raw_key) !=
      ERROR_SUCCESS) {
    return std::nullopt;
  }
  const auto close_key = [&]() { RegCloseKey(raw_key); };
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(raw_key, value_name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
      (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
    close_key();
    return std::nullopt;
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (RegQueryValueExW(raw_key, value_name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()),
                       &bytes) != ERROR_SUCCESS) {
    close_key();
    return std::nullopt;
  }
  close_key();
  while (!value.empty() && value.back() == L'\0') value.pop_back();
  return value;
}

std::optional<std::wstring> ReadRegistryDefault(HKEY root, const std::wstring& subkey) {
  return ReadRegistryString(root, subkey, nullptr);
}

bool RegistryKeyExists(HKEY root, const std::wstring& subkey) {
  HKEY key = nullptr;
  const auto status = RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key);
  if (status == ERROR_SUCCESS) RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

bool IsCurrentBitnessDll(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  input.seekg(0x3c);
  uint32_t pe_offset = 0;
  input.read(reinterpret_cast<char*>(&pe_offset), sizeof(pe_offset));
  if (!input) return false;
  input.seekg(pe_offset);
  uint32_t signature = 0;
  uint16_t machine = 0;
  input.read(reinterpret_cast<char*>(&signature), sizeof(signature));
  input.read(reinterpret_cast<char*>(&machine), sizeof(machine));
  if (!input || signature != 0x00004550) return false;
#if defined(_WIN64)
  return machine == IMAGE_FILE_MACHINE_AMD64 || machine == IMAGE_FILE_MACHINE_ARM64;
#else
  return machine == IMAGE_FILE_MACHINE_I386;
#endif
}

bool IsLanguageProfileRegistered() {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninitialize = SUCCEEDED(init);
  ITfInputProcessorProfiles* profiles = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&profiles));
  if (FAILED(hr) || !profiles) {
    if (uninitialize) CoUninitialize();
    return false;
  }
  IEnumTfLanguageProfiles* enumerator = nullptr;
  hr = profiles->EnumLanguageProfiles(0x0411, &enumerator);
  bool found = false;
  if (SUCCEEDED(hr) && enumerator) {
    TF_LANGUAGEPROFILE profile{};
    ULONG fetched = 0;
    while (enumerator->Next(1, &profile, &fetched) == S_OK) {
      if (IsEqualGUID(profile.clsid, tsf::kTextServiceClsid) &&
          IsEqualGUID(profile.guidProfile, tsf::kTextServiceProfileGuid)) {
        found = true;
        break;
      }
    }
    enumerator->Release();
  }
  profiles->Release();
  if (uninitialize) CoUninitialize();
  return found;
}

bool IsHostRunning() {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return false;
  DWORD current_session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &current_session)) {
    CloseHandle(snapshot);
    return false;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool found = false;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, L"azookey_inference_host.exe") == 0) {
        DWORD process_session = 0;
        if (ProcessIdToSessionId(entry.th32ProcessID, &process_session) &&
            process_session == current_session) {
          found = true;
          break;
        }
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return found;
}

std::string EnvironmentSummary() {
  SYSTEM_INFO system{};
  GetNativeSystemInfo(&system);
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  GlobalMemoryStatusEx(&memory);

  std::ostringstream output;
  RTL_OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  const auto ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto rtl_get_version =
      ntdll ? reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion"))
            : nullptr;
  output << "os=Windows";
  if (rtl_get_version && rtl_get_version(&version) == 0) {
    output << ' ' << version.dwMajorVersion << '.' << version.dwMinorVersion << '.'
           << version.dwBuildNumber;
  }
  output << '\n';
  output << "architecture=";
  switch (system.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
      output << "x64";
      break;
    case PROCESSOR_ARCHITECTURE_ARM64:
      output << "arm64";
      break;
    case PROCESSOR_ARCHITECTURE_INTEL:
      output << "x86";
      break;
    default:
      output << "unknown";
      break;
  }
  output << "\nlogical_processors=" << system.dwNumberOfProcessors;
  if (const auto processor = ReadRegistryString(
          HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
          L"ProcessorNameString")) {
    output << "\nprocessor=" << WideToUtf8(*processor);
  }
  output << "\nram_mb=" << (memory.ullTotalPhys / (1024ULL * 1024ULL)) << "\n";
  bool found_gpu = false;
  for (DWORD index = 0;; ++index) {
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (!EnumDisplayDevicesW(nullptr, index, &device, 0)) break;
    if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0 ||
        (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0 ||
        device.DeviceString[0] == L'\0') {
      continue;
    }
    output << "gpu=" << WideToUtf8(device.DeviceString) << "\n";
    found_gpu = true;
  }
  if (!found_gpu) output << "gpu=unknown\n";
  return output.str();
}
#else
bool IsHostRunning() { return false; }
std::string EnvironmentSummary() { return "os=unsupported\n"; }
#endif

void ProbeRegistration(Snapshot& snapshot) {
#ifdef _WIN32
  const auto clsid = GuidString(tsf::kTextServiceClsid);
  const auto profile = GuidString(tsf::kTextServiceProfileGuid);
  const std::wstring inproc_key = L"SOFTWARE\\Classes\\CLSID\\" + clsid + L"\\InprocServer32";
  const auto registered = ReadRegistryDefault(HKEY_LOCAL_MACHINE, inproc_key);
  snapshot.tip_path_registered = registered && !registered->empty();
  if (registered) {
    snapshot.tip_path = std::filesystem::path(*registered).string();
    std::error_code ec;
    snapshot.tip_path_exists = std::filesystem::is_regular_file(*registered, ec) && !ec;
    snapshot.tip_bitness_matches =
        snapshot.tip_path_exists && IsCurrentBitnessDll(std::filesystem::path(*registered));
  }
  const std::wstring profile_key =
      L"SOFTWARE\\Microsoft\\CTF\\TIP\\" + clsid + L"\\LanguageProfile\\0x00000411\\" + profile;
  snapshot.com_registration_matches =
      snapshot.tip_path_registered && RegistryKeyExists(HKEY_LOCAL_MACHINE, profile_key);
  snapshot.language_profile_registered = IsLanguageProfileRegistered();
#endif
}

std::string HandshakeToken() { return EnvironmentValue("AZOOKEY_IPC_HANDSHAKE_TOKEN"); }

void ProbeIpc(Snapshot& snapshot, std::string* host_health_json, std::string* ping_json) {
  ipc::NamedPipeClient client;
  if (!client.Connect(ipc::DefaultPipeName(), kConnectTimeoutMs)) return;

  ipc::HandshakeRequest handshake;
  handshake.tip_version = "azookey-diag/1";
  handshake.protocol_version = 1;
  handshake.capabilities = {"ping", "query_diagnostics"};
  handshake.client_id = "azookey-diag-" + std::to_string(
#ifdef _WIN32
                                              GetCurrentProcessId()
#else
                                              0
#endif
                                          );
  handshake.handshake_token = HandshakeToken();
  ipc::Envelope request;
  request.request_id = 1;
  request.trace_id = "diag-handshake";
  request.type = ipc::MessageType::Handshake;
  request.payload_json = ipc::BuildHandshakeRequest(handshake);
  if (!client.Send(request)) return;
  const auto response = client.ReceiveWithTimeout(kHandshakeTimeoutMs);
  const auto payload =
      response ? ipc::ParseHandshakeResponse(response->payload_json) : std::nullopt;
  if (!payload || !payload->accepted) return;
  snapshot.handshake_ok = true;

  const auto ping_started = std::chrono::steady_clock::now();
  ipc::PingPayload ping;
  ping.nonce = NowMs();
  ping.t_ms = NowMs();
  request.request_id = 2;
  request.trace_id = "diag-ping";
  request.type = ipc::MessageType::Ping;
  request.payload_json = ipc::BuildPing(ping);
  if (client.Send(request)) {
    const auto ping_response = client.ReceiveWithTimeout(kPingTimeoutMs);
    const auto parsed = ping_response ? ipc::ParsePing(ping_response->payload_json) : std::nullopt;
    if (parsed && parsed->nonce == ping.nonce) {
      snapshot.ping_responded = true;
      snapshot.ping_rtt_ms =
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - ping_started)
                                    .count());
      j::Object output;
      output.emplace("nonce", j::Value(parsed->nonce));
      output.emplace("rtt_ms", j::Value(snapshot.ping_rtt_ms));
      output.emplace("status", j::Value("ok"));
      *ping_json = JsonObject(std::move(output));
    }
  }

  request.request_id = 3;
  request.trace_id = "diag-query-diagnostics";
  request.type = ipc::MessageType::QueryDiagnostics;
  request.payload_json = "{}";
  if (!client.Send(request)) return;
  const auto diagnostics_response = client.ReceiveWithTimeout(kDiagnosticsTimeoutMs);
  const auto diagnostics = diagnostics_response
                               ? ipc::ParseQueryDiagnostics(diagnostics_response->payload_json)
                               : std::nullopt;
  if (diagnostics) {
    snapshot.host_diagnostics = *diagnostics;
    *host_health_json = ipc::BuildQueryDiagnostics(*diagnostics);
  }
}

uint32_t Crc32(std::string_view data) {
  uint32_t crc = 0xFFFFFFFFU;
  for (const unsigned char byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

void WriteU16(std::ostream& output, uint16_t value) {
  const char bytes[2] = {static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF)};
  output.write(bytes, sizeof(bytes));
}

void WriteU32(std::ostream& output, uint32_t value) {
  const char bytes[4] = {static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF),
                         static_cast<char>((value >> 16) & 0xFF),
                         static_cast<char>((value >> 24) & 0xFF)};
  output.write(bytes, sizeof(bytes));
}

std::pair<uint16_t, uint16_t> CurrentDosDateTime() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  const int year = std::clamp(local.tm_year + 1900, 1980, 2107);
  const uint16_t date =
      static_cast<uint16_t>(((year - 1980) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
  const uint16_t time =
      static_cast<uint16_t>((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
  return {time, date};
}

}  // namespace

bool ProbeSettingsFile(const std::filesystem::path& path) { return ProbeSettings(path).valid; }

bool ProbeLearningStoreFile(const std::filesystem::path& path, uint64_t* entries) {
  return ProbeLearningStore(path, entries);
}

bool ProbeUserDictionaryFile(const std::filesystem::path& path, uint64_t* entries,
                             uint64_t* skipped_entries) {
  return ProbeUserDictionary(path, entries, skipped_entries);
}

bool EmbeddedSettingsSchemaUsesOnlySupportedKeywords() {
  const auto schema = j::Parse(kSettingsSchemaJson);
  return schema && SchemaUsesOnlySupportedKeywords(*schema);
}

std::string StatusName(Status status) {
  switch (status) {
    case Status::Ok:
      return "ok";
    case Status::Warning:
      return "warning";
    case Status::Error:
      return "error";
  }
  return "error";
}

Report EvaluateSnapshot(const Snapshot& snapshot, uint64_t timestamp_ms) {
  Report report;
  report.timestamp_ms = timestamp_ms;

  const Status tip_status =
      snapshot.tip_path_registered && snapshot.tip_path_exists && snapshot.tip_bitness_matches
          ? Status::Ok
          : Status::Error;
  AddCheck(report, "D-001", "tip_dll", tip_status,
           tip_status == Status::Ok ? "TIP DLL is present and matches process bitness"
                                    : "TIP DLL is missing or has incompatible bitness",
           {{"path", j::Value(RedactFreeText(snapshot.tip_path))},
            {"registered", j::Value(snapshot.tip_path_registered)},
            {"exists", j::Value(snapshot.tip_path_exists)},
            {"bitness_matches", j::Value(snapshot.tip_bitness_matches)}});

  AddCheck(report, "D-002", "com_registration",
           snapshot.com_registration_matches ? Status::Ok : Status::Error,
           snapshot.com_registration_matches ? "COM and profile registration match"
                                             : "Required COM or profile registration is missing",
           {{"matches", j::Value(snapshot.com_registration_matches)}});

  AddCheck(report, "D-003", "language_profile",
           snapshot.language_profile_registered ? Status::Ok : Status::Error,
           snapshot.language_profile_registered ? "Japanese language profile is registered"
                                                : "Japanese language profile is not registered",
           {{"langid", j::Value("0x0411")},
            {"registered", j::Value(snapshot.language_profile_registered)}});

  AddCheck(report, "D-004", "host_process", snapshot.host_running ? Status::Ok : Status::Error,
           snapshot.host_running ? "Inference Host process is running"
                                 : "Inference Host process is not running",
           {{"running", j::Value(snapshot.host_running)}});

  AddCheck(report, "D-005", "ipc_handshake", snapshot.handshake_ok ? Status::Ok : Status::Error,
           snapshot.handshake_ok ? "IPC handshake is ready" : "IPC handshake failed",
           {{"ready", j::Value(snapshot.handshake_ok)}});

  Status ping_status = Status::Error;
  if (snapshot.ping_responded) {
    ping_status = snapshot.ping_rtt_ms <= 100
                      ? Status::Ok
                      : (snapshot.ping_rtt_ms <= 500 ? Status::Warning : Status::Error);
  }
  AddCheck(report, "D-006", "ipc_ping", ping_status,
           !snapshot.ping_responded
               ? "IPC ping did not respond"
               : (ping_status == Status::Ok ? "IPC ping latency is within threshold"
                                            : "IPC ping latency exceeds the preferred threshold"),
           {{"responded", j::Value(snapshot.ping_responded)},
            {"rtt_ms", j::Value(snapshot.ping_rtt_ms)}});

  Status model_path_status = Status::Ok;
  std::string model_path_message = "Model is disabled";
  if (snapshot.model_enabled) {
    if (snapshot.selected_model_path.empty()) {
      model_path_status = Status::Warning;
      model_path_message = "Model is enabled but no model is selected";
    } else if (!snapshot.selected_model_exists) {
      model_path_status = Status::Error;
      model_path_message = "Configured model path does not exist";
    } else {
      model_path_message = "Configured model path exists";
    }
  }
  AddCheck(report, "D-007", "model_path", model_path_status, model_path_message,
           {{"enabled", j::Value(snapshot.model_enabled)},
            {"selected", j::Value(!snapshot.selected_model_path.empty())},
            {"path", j::Value(RedactFreeText(snapshot.selected_model_path))},
            {"exists", j::Value(snapshot.selected_model_exists)}});

  Status validation_status = Status::Ok;
  std::string validation_message = "Model validation is not applicable";
  bool loaded = false;
  std::string engine;
  if (snapshot.host_diagnostics) {
    engine = snapshot.host_diagnostics->engine;
    loaded = snapshot.host_diagnostics->model_loaded &&
             snapshot.host_diagnostics->loaded_model_path.has_value() &&
             PathsEquivalent(snapshot.selected_model_path,
                             *snapshot.host_diagnostics->loaded_model_path);
  }
  if (snapshot.model_enabled && !snapshot.selected_model_path.empty()) {
    if (!snapshot.selected_model_valid) {
      validation_status = Status::Error;
      validation_message = "Selected model failed format validation";
    } else if (engine == "mock") {
      validation_status = Status::Warning;
      validation_message = "Selected model is valid but the effective runtime is mock";
      loaded = false;
    } else if (!loaded) {
      validation_status = Status::Warning;
      validation_message = "Selected model is valid but is not loaded";
    } else {
      validation_message = "Selected model is valid and loaded";
    }
  }
  AddCheck(report, "D-008", "model_validation", validation_status, validation_message,
           {{"valid", j::Value(snapshot.selected_model_valid)},
            {"loaded", j::Value(loaded)},
            {"engine", j::Value(engine)}});

  std::string fallback_state = snapshot.model_enabled ? "degraded_simple" : "healthy";
  if (snapshot.host_diagnostics) fallback_state = snapshot.host_diagnostics->fallback_state;
  Status fallback_status = Status::Ok;
  if (fallback_state == "safe_mode") {
    fallback_status = Status::Error;
  } else if (snapshot.model_enabled &&
             (fallback_state == "degraded_simple" || fallback_state == "degraded_model")) {
    fallback_status = Status::Warning;
  }
  AddCheck(report, "D-009", "fallback_state", fallback_status,
           fallback_status == Status::Ok
               ? "Runtime fallback state is healthy or intentionally disabled"
               : (fallback_status == Status::Warning ? "Runtime is operating in degraded mode"
                                                     : "Runtime is in safe mode"),
           {{"state", j::Value(fallback_state)}});

  AddCheck(report, "D-010", "learning_store",
           snapshot.learning_store_valid ? Status::Ok : Status::Error,
           snapshot.learning_store_valid ? "Learning store is readable"
                                         : "Learning store is unreadable or corrupt",
           {{"entries", j::Value(snapshot.learning_entries)}});

  AddCheck(report, "D-011", "user_dictionary",
           snapshot.user_dict_valid ? Status::Ok : Status::Error,
           snapshot.user_dict_valid ? "User dictionary is readable"
                                    : "User dictionary is unreadable or corrupt",
           {{"entries", j::Value(snapshot.user_dict_entries)},
            {"skipped_entries", j::Value(snapshot.user_dict_skipped_entries)}});

  AddCheck(report, "D-012", "settings", snapshot.settings_valid ? Status::Ok : Status::Error,
           snapshot.settings_valid
               ? (snapshot.settings_missing ? "Settings file is absent; defaults apply"
                                            : "Settings conform to the embedded schema")
               : "Settings failed schema validation",
           {{"missing", j::Value(snapshot.settings_missing)},
            {"valid", j::Value(snapshot.settings_valid)}});

  return report;
}

std::string SerializeReport(const Report& report) {
  j::Array checks;
  checks.reserve(report.checks.size());
  for (const auto& check : report.checks) {
    j::Object item;
    item.emplace("id", j::Value(check.id));
    item.emplace("name", j::Value(check.name));
    item.emplace("status", j::Value(StatusName(check.status)));
    item.emplace("message", j::Value(check.message));
    const auto details = j::Parse(check.details_json);
    item.emplace("details", details.value_or(j::Value(j::Object{})));
    checks.emplace_back(j::Value(std::move(item)));
  }
  j::Object root;
  root.emplace("checks", j::Value(std::move(checks)));
  root.emplace("status", j::Value(StatusName(report.status)));
  root.emplace("timestamp_ms", j::Value(report.timestamp_ms));
  return j::Stringify(j::Value(std::move(root)));
}

ProbeResult ProbeSystem() {
  ProbeResult result;
  Snapshot snapshot;
  ProbeRegistration(snapshot);
  snapshot.host_running = IsHostRunning();

  host::UserDataPathInputs inputs;
  inputs.local_app_data = host::GetPlatformLocalAppData();
  const auto paths = host::ResolveUserDataPaths(inputs);
  if (paths) {
    result.settings_path = paths->settings_path;
    const auto settings = ProbeSettings(paths->settings_path);
    snapshot.settings_valid = settings.valid;
    snapshot.settings_missing = settings.missing;
    snapshot.model_enabled = settings.model_enabled;
    snapshot.selected_model_path = settings.selected_path;
    const auto model_path = ExpandEnvironmentPath(settings.selected_path);
    std::error_code ec;
    snapshot.selected_model_exists =
        !model_path.empty() && std::filesystem::exists(model_path, ec) && !ec;
    snapshot.selected_model_valid = snapshot.selected_model_exists && ValidateModel(model_path);
    snapshot.learning_store_valid =
        ProbeLearningStore(paths->learning_path, &snapshot.learning_entries);
    snapshot.user_dict_valid = ProbeUserDictionary(
        paths->user_dict_path, &snapshot.user_dict_entries, &snapshot.user_dict_skipped_entries);
  } else {
    snapshot.settings_valid = false;
    snapshot.settings_missing = false;
    snapshot.learning_store_valid = false;
    snapshot.user_dict_valid = false;
  }

  ProbeIpc(snapshot, &result.host_health_json, &result.ipc_ping_json);
  result.report = EvaluateSnapshot(snapshot, NowMs());
  return result;
}

std::string RedactSettingsJson(std::string_view json) {
  const auto value = j::Parse(json);
  if (!value) return "{}";
  return RedactFreeText(j::Stringify(RedactValue(*value)));
}

std::string RedactFreeText(std::string text) { return core::RedactFreeText(std::move(text)); }

std::vector<ArchiveEntry> BuildCollectionEntries(const ProbeResult& result) {
  std::vector<ArchiveEntry> entries;
  entries.push_back({"diag.json", RedactFreeText(SerializeReport(result.report))});
  const auto settings = ReadTextFile(result.settings_path);
  entries.push_back(
      {"settings.redacted.json", settings ? RedactSettingsJson(*settings) : std::string("{}")});
  entries.push_back({"host-health.json", result.host_health_json.empty()
                                             ? std::string("{\"available\":false}")
                                             : RedactFreeText(result.host_health_json)});
  entries.push_back({"ipc-ping.json", result.ipc_ping_json.empty()
                                          ? std::string("{\"available\":false}")
                                          : result.ipc_ping_json});
  entries.push_back({"environment.txt", EnvironmentSummary()});
  entries.push_back({"crash-summary.txt",
                     "Crash dump bodies are not included. No crash summary was collected.\n"});
  return entries;
}

bool WriteZip(const std::filesystem::path& output, const std::vector<ArchiveEntry>& entries,
              std::string* error) {
  struct CentralEntry {
    ArchiveEntry entry;
    uint32_t crc{};
    uint32_t offset{};
  };
  std::ofstream stream(output, std::ios::binary | std::ios::trunc);
  if (!stream) {
    if (error) *error = "failed to create output ZIP";
    return false;
  }
  const auto fail = [&](std::string_view message) {
    stream.close();
    std::error_code ec;
    std::filesystem::remove(output, ec);
    if (error) *error = std::string(message);
    return false;
  };
  const auto [dos_time, dos_date] = CurrentDosDateTime();
  std::vector<CentralEntry> central;
  central.reserve(entries.size());
  for (const auto& entry : entries) {
    if (entry.name.empty() || entry.name.find("..") != std::string::npos ||
        entry.name.find('\\') != std::string::npos ||
        entry.name.size() > std::numeric_limits<uint16_t>::max() ||
        entry.content.size() > std::numeric_limits<uint32_t>::max()) {
      return fail("invalid ZIP entry");
    }
    const auto offset = stream.tellp();
    if (offset < 0 || static_cast<uint64_t>(offset) > std::numeric_limits<uint32_t>::max()) {
      return fail("diagnostic ZIP is too large");
    }
    CentralEntry item{entry, Crc32(entry.content), static_cast<uint32_t>(offset)};
    WriteU32(stream, 0x04034B50);
    WriteU16(stream, 20);
    WriteU16(stream, 0x0800);
    WriteU16(stream, 0);
    WriteU16(stream, dos_time);
    WriteU16(stream, dos_date);
    WriteU32(stream, item.crc);
    WriteU32(stream, static_cast<uint32_t>(entry.content.size()));
    WriteU32(stream, static_cast<uint32_t>(entry.content.size()));
    WriteU16(stream, static_cast<uint16_t>(entry.name.size()));
    WriteU16(stream, 0);
    stream.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
    stream.write(entry.content.data(), static_cast<std::streamsize>(entry.content.size()));
    central.push_back(std::move(item));
  }
  const auto central_offset_raw = stream.tellp();
  if (central_offset_raw < 0 ||
      static_cast<uint64_t>(central_offset_raw) > std::numeric_limits<uint32_t>::max()) {
    return fail("diagnostic ZIP is too large");
  }
  const uint32_t central_offset = static_cast<uint32_t>(central_offset_raw);
  for (const auto& item : central) {
    WriteU32(stream, 0x02014B50);
    WriteU16(stream, 20);
    WriteU16(stream, 20);
    WriteU16(stream, 0x0800);
    WriteU16(stream, 0);
    WriteU16(stream, dos_time);
    WriteU16(stream, dos_date);
    WriteU32(stream, item.crc);
    WriteU32(stream, static_cast<uint32_t>(item.entry.content.size()));
    WriteU32(stream, static_cast<uint32_t>(item.entry.content.size()));
    WriteU16(stream, static_cast<uint16_t>(item.entry.name.size()));
    WriteU16(stream, 0);
    WriteU16(stream, 0);
    WriteU16(stream, 0);
    WriteU16(stream, 0);
    WriteU32(stream, 0);
    WriteU32(stream, item.offset);
    stream.write(item.entry.name.data(), static_cast<std::streamsize>(item.entry.name.size()));
  }
  const auto central_end_raw = stream.tellp();
  if (central_end_raw < 0 ||
      static_cast<uint64_t>(central_end_raw) > std::numeric_limits<uint32_t>::max() ||
      central.size() > std::numeric_limits<uint16_t>::max()) {
    return fail("diagnostic ZIP is too large");
  }
  const uint32_t central_size = static_cast<uint32_t>(central_end_raw) - central_offset;
  WriteU32(stream, 0x06054B50);
  WriteU16(stream, 0);
  WriteU16(stream, 0);
  WriteU16(stream, static_cast<uint16_t>(central.size()));
  WriteU16(stream, static_cast<uint16_t>(central.size()));
  WriteU32(stream, central_size);
  WriteU32(stream, central_offset);
  WriteU16(stream, 0);
  if (!stream) {
    return fail("failed while writing diagnostic ZIP");
  }
  stream.close();
  if (!stream) return fail("failed while writing diagnostic ZIP");
  return true;
}

}  // namespace azookey::diagnostics
