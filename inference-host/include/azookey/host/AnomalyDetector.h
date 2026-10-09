#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/ipc/Payloads.h"
#include "azookey/learning/Persona.h"

namespace azookey::host {

// rich-features-spec X-3-4 / X-3-6 (DEV-1532): semantic anomaly detection over
// a paragraph through the AI backend. The model is asked for the exact text of
// each suspicious span ("quote") rather than offsets, which language models do
// not count reliably; the Host locates the quote and reports UTF-16 offsets.

// The style hint appended to the instruction; empty without persona samples.
std::string AnomalyPersonaHint(const std::optional<learning::Persona>& persona);

// Parses the model's `result` string: a JSON array of
// {quote, reason, suggestions[], confidence}. Each quote is searched in `text`
// from the end of the previous match (then from the start), so repeated
// phrases map to successive occurrences. Unplaceable, empty or malformed items
// and exact duplicates of a placed span are dropped; overlapping spans are kept. nullopt when
// `result` is not a JSON array at all. At most `max_findings` findings, ordered by start.
std::optional<std::vector<ipc::AnomalyFindingField>> ParseAnomalyFindings(std::string_view text,
                                                                          std::string_view result,
                                                                          size_t max_findings);

// Number of UTF-16 code units `text` (UTF-8) occupies; an invalid byte counts as one.
uint32_t Utf16Length(std::string_view text);

}  // namespace azookey::host
