#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "azookey/ipc/Limits.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/ipc/ProofreadSession.h"

namespace azookey::settings {

// Why text pasted into the "校正" pane is not sent.
enum class ProofreadInputProblem { None, Empty, TooLarge };

// The Host refuses an empty text and one over kMaxAnomalyTextBytes (UTF-8 bytes); neither is sent.
ProofreadInputProblem CheckProofreadInput(std::string_view utf8_text);

// The request for text the user pasted into the settings app: it is the user's own text, so it is
// not from a secure context and learning is allowed (rich-features-spec X-3-6). The Host still
// refuses it unless the AI cleanup settings and consent allow an external backend.
azookey::ipc::DetectAnomaliesRequest MakeProofreadRequest(std::string utf8_text);

// The request for the document a TIP session handed over: the event privacy the TIP read from the
// application's context goes to the Host as it is, so the Host can still refuse it.
azookey::ipc::DetectAnomaliesRequest MakeSessionProofreadRequest(
    const azookey::ipc::proofread::DocumentMessage& document);

// What the pane shows when a session opens: "Proofread_Session_<status>". A blocked or unavailable
// session carries no text, so only this reason is shown.
std::string ProofreadSessionStatusResource(azookey::ipc::proofread::DocumentStatus status);

// The answer to "select this range in the application": "Proofread_SelectResult_<result>", or
// "Proofread_SelectResult_lost" when the session no longer answers.
std::string ProofreadSelectResultResource(
    std::optional<azookey::ipc::proofread::SelectResult> result);

// Error categories a DetectAnomalies response carries. Each has a "Proofread_Error_<code>" string;
// any other falls back to "Proofread_Error_other".
inline constexpr std::array<std::string_view, 5> kProofreadErrorCodes{
    "invalid_request", "not_authenticated", "unsupported", "blocked", "backend_failed"};

std::string ProofreadErrorResource(std::string_view error);

// Whether a finding's UTF-16 range lies inside `text`, is not empty, and does not start or end in
// the middle of a surrogate pair. Ranges may overlap each other; that is not checked here.
bool IsValidFindingRange(const std::wstring& text, uint32_t start, uint32_t length);

// The text a finding points at, cut to `max_units` code units (with "..." added), or nothing when
// the range is not valid. The text is shown as plain text only.
std::optional<std::wstring> FindingExcerpt(const std::wstring& text, uint32_t start,
                                           uint32_t length, size_t max_units = 40);

}  // namespace azookey::settings
