#include "azookey/host/AnomalyDetector.h"

#include <algorithm>
#include <cstdio>

#include "azookey/core/Utf8.h"
#include "azookey/ipc/Json.h"
#include "azookey/ipc/Limits.h"

namespace azookey::host {
namespace {
namespace j = ipc::json;

// A quote that occurs more than once cannot be placed with certainty: one this
// short is dropped, a longer one is kept with its confidence capped.
constexpr uint32_t kMinAmbiguousQuoteUnits = 3;
constexpr double kAmbiguousConfidence = 0.3;

size_t CountOccurrences(std::string_view text, std::string_view quote) {
  size_t count = 0;
  for (size_t at = text.find(quote); at != std::string_view::npos; at = text.find(quote, at + 1)) {
    ++count;
  }
  return count;
}

// Cuts at a code point boundary so the result stays valid UTF-8.
std::string Truncate(std::string text, size_t max_bytes) {
  if (text.size() <= max_bytes) return text;
  size_t end = 0;
  for (size_t pos = 0; pos < text.size();) {
    char32_t cp{};
    size_t next = pos;
    core::DecodeNextUtf8(text, next, cp);
    if (next > max_bytes) break;
    end = pos = next;
  }
  text.resize(end);
  return text;
}

std::string Ratio(double value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%.2f", value);
  return buffer;
}

}  // namespace

std::string AnomalyPersonaHint(const std::optional<learning::Persona>& persona) {
  if (!persona || persona->sample_count == 0) return {};
  return "The writer's usual style, as shares of their past commits: polite " +
         Ratio(persona->polite_ratio) + ", casual " + Ratio(persona->casual_ratio) +
         ", technical " + Ratio(persona->technical_ratio) + ", kaomoji " +
         Ratio(persona->kaomoji_ratio) + ". ";
}

uint32_t Utf16Length(std::string_view text) {
  uint32_t units = 0;
  for (size_t pos = 0; pos < text.size();) {
    char32_t cp{};
    // An invalid byte is consumed and counted as one unit.
    if (!core::DecodeNextUtf8(text, pos, cp)) cp = 0;
    units += cp > 0xFFFF ? 2 : 1;
  }
  return units;
}

std::optional<std::vector<ipc::AnomalyFindingField>> ParseAnomalyFindings(std::string_view text,
                                                                          std::string_view result,
                                                                          size_t max_findings) {
  const auto root = j::Parse(result);
  if (!root || !root->IsArray()) return std::nullopt;
  std::vector<ipc::AnomalyFindingField> findings;
  size_t search_from = 0;
  for (const auto& item : root->AsArray()) {
    if (findings.size() >= max_findings) break;
    if (!item.IsObject()) continue;
    const auto quote = item.GetString("quote");
    if (!quote || quote->empty()) continue;
    auto at = text.find(*quote, search_from);
    if (at == std::string_view::npos) at = text.find(*quote);
    if (at == std::string_view::npos) continue;
    search_from = at + quote->size();

    ipc::AnomalyFindingField finding;
    finding.start = Utf16Length(text.substr(0, at));
    finding.length = Utf16Length(*quote);
    const bool ambiguous = CountOccurrences(text, *quote) > 1;
    if (ambiguous && finding.length < kMinAmbiguousQuoteUnits) continue;
    // The same span twice (a quote found again from the start) is one finding;
    // overlapping spans are kept, each with its own reason.
    if (std::any_of(findings.begin(), findings.end(), [&](const auto& placed) {
          return placed.start == finding.start && placed.length == finding.length;
        })) {
      continue;
    }
    finding.reason =
        Truncate(item.GetString("reason").value_or(std::string()), ipc::kMaxAnomalyReasonBytes);
    finding.confidence = std::clamp(item.GetNumber("confidence").value_or(0.5), 0.0, 1.0);
    if (ambiguous) finding.confidence = std::min(finding.confidence, kAmbiguousConfidence);
    if (const auto* suggestions = item.GetArray("suggestions")) {
      for (const auto& suggestion : *suggestions) {
        if (finding.suggestions.size() >= ipc::kMaxAnomalySuggestions) break;
        if (suggestion.IsString() && !suggestion.AsString().empty() &&
            suggestion.AsString().size() <= ipc::kMaxAnomalySuggestionBytes &&
            suggestion.AsString() != *quote) {
          finding.suggestions.push_back(suggestion.AsString());
        }
      }
    }
    findings.push_back(std::move(finding));
  }
  std::stable_sort(findings.begin(), findings.end(),
                   [](const auto& a, const auto& b) { return a.start < b.start; });
  return findings;
}

}  // namespace azookey::host
