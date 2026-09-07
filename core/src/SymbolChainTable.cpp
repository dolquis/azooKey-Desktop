#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include "azookey/core/SymbolRewriter.h"

namespace azookey::core {
namespace {
// Independently defined Unicode bracket families; not derived from Mozc data.
// Keep pairs consistent with the built-in M61 bracket table.
struct Bracket {
  std::string_view open;
  std::string_view close;
  std::string_view name;
};
constexpr std::array<Bracket, 9> kBrackets{{
    {"「", "」", "かぎ括弧"},
    {"『", "』", "二重かぎ括弧"},
    {"【", "】", "隅付き括弧"},
    {"〔", "〕", "亀甲括弧"},
    {"（", "）", "丸括弧"},
    {"［", "］", "角括弧"},
    {"｛", "｝", "波括弧"},
    {"〈", "〉", "山括弧"},
    {"《", "》", "二重山括弧"},
}};

std::string Surface(const Bracket& bracket, int family) {
  if (family == 0) return std::string(bracket.open);
  if (family == 1) return std::string(bracket.close);
  return std::string(bracket.open) + std::string(bracket.close);
}
}  // namespace

void AppendSymbolChain(std::vector<Candidate>& candidates, const std::string& reading) {
  int family = -1;
  for (const auto& candidate : candidates) {
    for (int kind = 0; kind < 3 && family < 0; ++kind) {
      for (const auto& bracket : kBrackets) {
        if (candidate.surface == Surface(bracket, kind)) {
          family = kind;
          break;
        }
      }
    }
    if (family >= 0) break;
  }
  if (family < 0) return;

  for (const auto& bracket : kBrackets) {
    auto surface = Surface(bracket, family);
    if (std::any_of(candidates.begin(), candidates.end(),
                    [&](const Candidate& candidate) { return candidate.surface == surface; }))
      continue;
    Candidate candidate;
    candidate.surface = std::move(surface);
    candidate.reading = reading;
    candidate.source = CandidateSource::Symbol;
    candidate.description = (family == 0   ? "始め"
                             : family == 1 ? "終わり"
                                           : "") +
                            std::string(bracket.name);
    candidate.debug_info = "symbol-rewriter:chain";
    candidates.push_back(std::move(candidate));
  }
}

}  // namespace azookey::core
