#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/ipc/Limits.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

// The batch correction view's session between the TIP and the settings app
// (docs/rich-features-spec.md X-3-6). The TIP owns a one-shot named pipe, the
// settings app connects to it with the token from `--proofread-session`, the
// TIP sends one document message, and the settings app sends select requests
// that the TIP answers. Every message is a 4-byte little-endian length and a
// JSON object. Offsets are UTF-16 code units of the document text.
namespace azookey::ipc::proofread {

inline constexpr std::size_t kSessionTokenBytes = 16;
inline constexpr std::size_t kSessionTokenHexLength = kSessionTokenBytes * 2;
// A 60 KiB text escaped as JSON stays well below this.
inline constexpr uint32_t kMaxDocumentFrameBytes = 512 * 1024;
inline constexpr uint32_t kMaxRequestFrameBytes = 4 * 1024;

// 32 lowercase hexadecimal digits.
bool IsValidSessionToken(std::string_view token) noexcept;
std::wstring SessionPipeName(std::string_view token);

enum class DocumentStatus : uint8_t {
  Captured,
  // A secure or no-learning context; the TIP read no text.
  Blocked,
  // The application exposed no readable text.
  Unavailable,
};

struct DocumentMessage {
  DocumentStatus status{DocumentStatus::Unavailable};
  // UTF-8. Non-empty and at most kMaxAnomalyTextBytes when Captured, empty otherwise.
  std::string text;
  // The event privacy of the context the text came from, for DetectAnomalies.
  bool secure{true};
  bool learning_allowed{false};
  // The application's window, to bring it to the foreground before selecting.
  uint64_t window{0};
};

// A finding's suggestions offered with the reconversion candidates. They are
// model output, so they are bounded like DetectAnomalies' and used as text only.
inline constexpr std::size_t kMaxSelectSuggestions = kMaxAnomalySuggestions;
inline constexpr std::size_t kMaxSelectSuggestionBytes = kMaxAnomalySuggestionBytes;

// Non-empty valid UTF-8 of at most kMaxSelectSuggestionBytes without control
// characters, line or paragraph separators, bidirectional controls, a BOM or
// interlinear annotation characters.
bool IsValidSelectSuggestion(std::string_view utf8) noexcept;

struct SelectRequest {
  uint64_t start{0};
  uint64_t length{0};
  // Also open reconversion candidates for the selected range. Missing reads
  // as false, so a TIP that does not know it only selects.
  bool reconvert{false};
  // At most kMaxSelectSuggestions, each passing IsValidSelectSuggestion; the
  // codec drops any other element on both sides. Missing reads as none.
  std::vector<std::string> suggestions;
};

enum class SelectResult : uint8_t {
  Ok,
  // The text in the window differs from what was checked.
  DocumentChanged,
  // The document is gone or refused the edit session.
  ContextGone,
  // Out of the window, or inside a surrogate pair.
  InvalidRange,
  Failed,
};

std::string BuildDocumentMessage(const DocumentMessage& message);
std::optional<DocumentMessage> ParseDocumentMessage(std::string_view json);
std::string BuildSelectRequest(const SelectRequest& request);
std::optional<SelectRequest> ParseSelectRequest(std::string_view json);
std::string BuildSelectResponse(SelectResult result);
std::optional<SelectResult> ParseSelectResponse(std::string_view json);
std::string_view SelectResultName(SelectResult result) noexcept;

// Length prefix plus body; std::nullopt when the body exceeds `max_bytes`.
std::optional<std::vector<uint8_t>> EncodeFrame(std::string_view json, uint32_t max_bytes);
// The body length a 4-byte header announces; std::nullopt for 0 or above `max_bytes`.
std::optional<uint32_t> DecodeFrameLength(const uint8_t (&header)[4], uint32_t max_bytes) noexcept;

#ifdef _WIN32
// A new random token from the system RNG, or std::nullopt.
std::optional<std::string> GenerateSessionToken();

// The server end of a session pipe: one instance, first instance only, local
// clients only, overlapped, a DACL that admits the current logon SID only, and
// a medium mandatory label that refuses lower-integrity processes.
// INVALID_HANDLE_VALUE on failure (also when the logon SID cannot be read).
HANDLE CreateSessionPipe(std::string_view token);
#endif

}  // namespace azookey::ipc::proofread
