#include "ProofreadModel.h"

#include <algorithm>
#include <utility>

namespace azookey::settings {
namespace {

bool IsHighSurrogate(wchar_t unit) { return unit >= 0xD800 && unit <= 0xDBFF; }
bool IsLowSurrogate(wchar_t unit) { return unit >= 0xDC00 && unit <= 0xDFFF; }

}  // namespace

ProofreadInputProblem CheckProofreadInput(std::string_view utf8_text) {
  if (utf8_text.empty()) return ProofreadInputProblem::Empty;
  if (utf8_text.size() > azookey::ipc::kMaxAnomalyTextBytes) return ProofreadInputProblem::TooLarge;
  return ProofreadInputProblem::None;
}

azookey::ipc::DetectAnomaliesRequest MakeProofreadRequest(std::string utf8_text) {
  azookey::ipc::DetectAnomaliesRequest request;
  request.text = std::move(utf8_text);
  request.secure = false;
  request.learning_allowed = true;
  return request;
}

azookey::ipc::DetectAnomaliesRequest MakeSessionProofreadRequest(
    const azookey::ipc::proofread::DocumentMessage& document) {
  azookey::ipc::DetectAnomaliesRequest request;
  request.text = document.text;
  request.secure = document.secure;
  request.learning_allowed = document.learning_allowed;
  return request;
}

std::string ProofreadSessionStatusResource(azookey::ipc::proofread::DocumentStatus status) {
  using azookey::ipc::proofread::DocumentStatus;
  switch (status) {
    case DocumentStatus::Captured:
      return "Proofread_Session_captured";
    case DocumentStatus::Blocked:
      return "Proofread_Session_blocked";
    case DocumentStatus::Unavailable:
      break;
  }
  return "Proofread_Session_unavailable";
}

std::string ProofreadSelectResultResource(
    std::optional<azookey::ipc::proofread::SelectResult> result) {
  if (!result) return "Proofread_SelectResult_lost";
  return "Proofread_SelectResult_" +
         std::string(azookey::ipc::proofread::SelectResultName(*result));
}

std::string ProofreadErrorResource(std::string_view error) {
  const bool known = std::find(kProofreadErrorCodes.begin(), kProofreadErrorCodes.end(), error) !=
                     kProofreadErrorCodes.end();
  if (!known) return "Proofread_Error_other";
  return "Proofread_Error_" + std::string(error);
}

bool IsValidFindingRange(const std::wstring& text, uint32_t start, uint32_t length) {
  if (length == 0) return false;
  const uint64_t end = static_cast<uint64_t>(start) + length;
  if (end > text.size()) return false;
  // A boundary between the two halves of a surrogate pair does not name a character.
  if (start > 0 && IsHighSurrogate(text[start - 1]) && IsLowSurrogate(text[start])) return false;
  if (end < text.size() && IsHighSurrogate(text[end - 1]) && IsLowSurrogate(text[end])) {
    return false;
  }
  return true;
}

std::optional<std::wstring> FindingExcerpt(const std::wstring& text, uint32_t start,
                                           uint32_t length, size_t max_units) {
  if (!IsValidFindingRange(text, start, length)) return std::nullopt;
  if (length <= max_units) return text.substr(start, length);
  size_t cut = max_units;
  // Do not cut between the halves of a surrogate pair.
  if (cut > 0 && IsHighSurrogate(text[start + cut - 1])) --cut;
  return text.substr(start, cut) + L"...";
}

}  // namespace azookey::settings
