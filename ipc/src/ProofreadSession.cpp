#include "azookey/ipc/ProofreadSession.h"

#include <array>
#include <utility>

#include "azookey/ipc/Json.h"
#include "azookey/ipc/Limits.h"

#ifdef _WIN32
#include <bcrypt.h>
#include <sddl.h>

#include "NamedPipeTransportInternal.h"
#endif

namespace azookey::ipc::proofread {
namespace {

namespace j = azookey::ipc::json;

constexpr std::string_view kKindDocument = "document";
constexpr std::string_view kKindSelect = "select";
constexpr std::string_view kKindSelectResult = "select_result";

std::optional<j::Value> ParseKind(std::string_view json, std::string_view kind) {
  auto value = j::Parse(json);
  if (!value || !value->IsObject() || value->GetString("kind") != std::string(kind))
    return std::nullopt;
  return value;
}

std::string_view DocumentStatusName(DocumentStatus status) {
  switch (status) {
    case DocumentStatus::Captured:
      return "captured";
    case DocumentStatus::Blocked:
      return "blocked";
    case DocumentStatus::Unavailable:
      break;
  }
  return "unavailable";
}

std::optional<DocumentStatus> DocumentStatusFromName(std::string_view name) {
  if (name == "captured") return DocumentStatus::Captured;
  if (name == "blocked") return DocumentStatus::Blocked;
  if (name == "unavailable") return DocumentStatus::Unavailable;
  return std::nullopt;
}

// The next code point of valid UTF-8 (shortest form, no surrogates), or
// std::nullopt; `pos` moves past it.
std::optional<char32_t> NextCodePoint(std::string_view text, std::size_t& pos) noexcept {
  const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
  const unsigned char first = byte(pos);
  std::size_t length = 0;
  char32_t code = 0;
  char32_t minimum = 0;
  if (first < 0x80) {
    ++pos;
    return first;
  } else if ((first & 0xE0) == 0xC0) {
    length = 2, code = first & 0x1F, minimum = 0x80;
  } else if ((first & 0xF0) == 0xE0) {
    length = 3, code = first & 0x0F, minimum = 0x800;
  } else if ((first & 0xF8) == 0xF0) {
    length = 4, code = first & 0x07, minimum = 0x10000;
  } else {
    return std::nullopt;
  }
  if (text.size() - pos < length) return std::nullopt;
  for (std::size_t i = 1; i < length; ++i) {
    if ((byte(pos + i) & 0xC0) != 0x80) return std::nullopt;
    code = (code << 6) | (byte(pos + i) & 0x3F);
  }
  if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return std::nullopt;
  pos += length;
  return code;
}

// Characters that would change how the candidate is laid out or read, or that
// do not belong in a one-line replacement.
bool IsRefusedSuggestionCodePoint(char32_t code) noexcept {
  return code < 0x20 || (code >= 0x7F && code <= 0x9F) || code == 0x061C || code == 0x200E ||
         code == 0x200F || code == 0x2028 || code == 0x2029 || (code >= 0x202A && code <= 0x202E) ||
         (code >= 0x2066 && code <= 0x2069) || code == 0xFEFF || (code >= 0xFFF9 && code <= 0xFFFB);
}

std::vector<std::string> ValidSuggestions(const std::vector<std::string>& suggestions) {
  std::vector<std::string> valid;
  for (const auto& suggestion : suggestions) {
    if (valid.size() >= kMaxSelectSuggestions) break;
    if (IsValidSelectSuggestion(suggestion)) valid.push_back(suggestion);
  }
  return valid;
}

constexpr std::array kSelectResults{SelectResult::Ok, SelectResult::DocumentChanged,
                                    SelectResult::ContextGone, SelectResult::InvalidRange,
                                    SelectResult::Failed};

}  // namespace

bool IsValidSessionToken(std::string_view token) noexcept {
  if (token.size() != kSessionTokenHexLength) return false;
  for (const char c : token) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

std::wstring SessionPipeName(std::string_view token) {
  std::wstring name = L"\\\\.\\pipe\\azookey-proofread-";
  name.append(token.begin(), token.end());
  return name;
}

std::string BuildDocumentMessage(const DocumentMessage& message) {
  j::Object o;
  o.emplace("kind", j::Value(std::string(kKindDocument)));
  o.emplace("status", j::Value(std::string(DocumentStatusName(message.status))));
  o.emplace("text",
            j::Value(message.status == DocumentStatus::Captured ? message.text : std::string()));
  o.emplace("secure", j::Value(message.secure));
  o.emplace("learning_allowed", j::Value(message.learning_allowed));
  o.emplace("window", j::Value(message.window));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<DocumentMessage> ParseDocumentMessage(std::string_view json) {
  const auto value = ParseKind(json, kKindDocument);
  if (!value) return std::nullopt;
  const auto status_name = value->GetString("status");
  const auto status = status_name ? DocumentStatusFromName(*status_name) : std::nullopt;
  auto text = value->GetString("text");
  if (!status || !text) return std::nullopt;
  if (*status == DocumentStatus::Captured ? text->empty() || text->size() > kMaxAnomalyTextBytes
                                          : !text->empty())
    return std::nullopt;
  DocumentMessage message;
  message.status = *status;
  message.text = std::move(*text);
  // Missing privacy reads as the most restrictive, like DetectAnomalies.
  message.secure = value->GetBool("secure").value_or(true);
  message.learning_allowed = value->GetBool("learning_allowed").value_or(false);
  message.window = value->GetUInt("window").value_or(0);
  return message;
}

bool IsValidSelectSuggestion(std::string_view utf8) noexcept {
  if (utf8.empty() || utf8.size() > kMaxSelectSuggestionBytes) return false;
  for (std::size_t pos = 0; pos < utf8.size();) {
    const auto code = NextCodePoint(utf8, pos);
    if (!code || IsRefusedSuggestionCodePoint(*code)) return false;
  }
  return true;
}

std::string BuildSelectRequest(const SelectRequest& request) {
  j::Object o;
  o.emplace("kind", j::Value(std::string(kKindSelect)));
  o.emplace("start", j::Value(request.start));
  o.emplace("length", j::Value(request.length));
  o.emplace("reconvert", j::Value(request.reconvert));
  // Only valid suggestions are sent, so the request stays within kMaxRequestFrameBytes.
  auto suggestions = ValidSuggestions(request.suggestions);
  if (!suggestions.empty()) {
    j::Array items;
    for (auto& suggestion : suggestions) items.emplace_back(std::move(suggestion));
    o.emplace("suggestions", j::Value(std::move(items)));
  }
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<SelectRequest> ParseSelectRequest(std::string_view json) {
  const auto value = ParseKind(json, kKindSelect);
  if (!value) return std::nullopt;
  const auto start = value->GetUInt("start");
  const auto length = value->GetUInt("length");
  if (!start || !length) return std::nullopt;
  SelectRequest request{*start, *length, value->GetBool("reconvert").value_or(false), {}};
  // A malformed element is dropped on its own; it does not end the session.
  if (const auto* suggestions = value->GetArray("suggestions")) {
    for (const auto& suggestion : *suggestions) {
      if (request.suggestions.size() >= kMaxSelectSuggestions) break;
      if (suggestion.IsString() && IsValidSelectSuggestion(suggestion.AsString()))
        request.suggestions.push_back(suggestion.AsString());
    }
  }
  return request;
}

std::string_view SelectResultName(SelectResult result) noexcept {
  switch (result) {
    case SelectResult::Ok:
      return "ok";
    case SelectResult::DocumentChanged:
      return "document_changed";
    case SelectResult::ContextGone:
      return "context_gone";
    case SelectResult::InvalidRange:
      return "invalid_range";
    case SelectResult::Failed:
      break;
  }
  return "failed";
}

std::string BuildSelectResponse(SelectResult result) {
  j::Object o;
  o.emplace("kind", j::Value(std::string(kKindSelectResult)));
  o.emplace("result", j::Value(std::string(SelectResultName(result))));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<SelectResult> ParseSelectResponse(std::string_view json) {
  const auto value = ParseKind(json, kKindSelectResult);
  if (!value) return std::nullopt;
  const auto name = value->GetString("result");
  if (!name) return std::nullopt;
  for (const auto result : kSelectResults) {
    if (*name == SelectResultName(result)) return result;
  }
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> EncodeFrame(std::string_view json, uint32_t max_bytes) {
  if (json.empty() || json.size() > max_bytes) return std::nullopt;
  const auto size = static_cast<uint32_t>(json.size());
  std::vector<uint8_t> bytes;
  bytes.reserve(4 + json.size());
  for (int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<uint8_t>(size >> shift));
  bytes.insert(bytes.end(), json.begin(), json.end());
  return bytes;
}

std::optional<uint32_t> DecodeFrameLength(const uint8_t (&header)[4], uint32_t max_bytes) noexcept {
  const uint32_t size = static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
                        (static_cast<uint32_t>(header[2]) << 16) |
                        (static_cast<uint32_t>(header[3]) << 24);
  if (size == 0 || size > max_bytes) return std::nullopt;
  return size;
}

#ifdef _WIN32
namespace {

std::wstring CurrentLogonSid() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
  DWORD required = 0;
  GetTokenInformation(token, TokenGroups, nullptr, 0, &required);
  std::vector<uint8_t> buffer(required);
  const bool ok =
      required > 0 && GetTokenInformation(token, TokenGroups, buffer.data(), required, &required);
  CloseHandle(token);
  if (!ok) return {};
  const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(buffer.data());
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID) continue;
    wchar_t* sid = nullptr;
    if (!ConvertSidToStringSidW(groups->Groups[i].Sid, &sid)) return {};
    std::wstring result(sid);
    LocalFree(sid);
    return result;
  }
  return {};
}

}  // namespace

std::optional<std::string> GenerateSessionToken() {
  std::array<uint8_t, kSessionTokenBytes> bytes{};
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
    return std::nullopt;
  constexpr char kHex[] = "0123456789abcdef";
  std::string token;
  token.reserve(kSessionTokenHexLength);
  for (const uint8_t b : bytes) {
    token.push_back(kHex[b >> 4]);
    token.push_back(kHex[b & 0x0f]);
  }
  return token;
}

HANDLE CreateSessionPipe(std::string_view token) {
  if (!IsValidSessionToken(token)) return INVALID_HANDLE_VALUE;
  // Unlike the Host pipe there is no fallback: without the logon SID the
  // session does not start.
  const auto logon_sid = CurrentLogonSid();
  if (logon_sid.empty()) return INVALID_HANDLE_VALUE;
  // The medium mandatory label keeps a lower-integrity process of the same
  // logon from reading, writing or running the pipe (and from using up the
  // TIP's refusals of unexpected clients).
  const auto sddl = internal::BuildPipeSecuritySddl(logon_sid, false) + L"S:(ML;;NRNWNX;;;ME)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                            &descriptor, nullptr))
    return INVALID_HANDLE_VALUE;
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  const auto name = SessionPipeName(token);
  const HANDLE pipe = CreateNamedPipeW(
      name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
      kMaxDocumentFrameBytes, kMaxRequestFrameBytes, 0, &attributes);
  LocalFree(descriptor);
  return pipe;
}
#endif

}  // namespace azookey::ipc::proofread
