#include "runner/CaseSelection.h"

#include <algorithm>

namespace azookey::compat_test {
namespace {

bool IsCaseIdFormat(std::wstring_view id) {
  return id.size() == 5 && id[0] == L'C' && id[1] == L'-' &&
         std::all_of(id.begin() + 2, id.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
}

bool Contains(const std::vector<std::string>& values, std::string_view value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

std::optional<std::vector<std::string>> ParseCaseIdList(std::wstring_view text) {
  std::vector<std::string> ids;
  size_t start = 0;
  while (true) {
    const size_t comma = text.find(L',', start);
    const auto item =
        text.substr(start, comma == std::wstring_view::npos ? text.npos : comma - start);
    if (!IsCaseIdFormat(item)) return std::nullopt;
    std::string id;
    for (const wchar_t ch : item) id.push_back(static_cast<char>(ch));
    if (Contains(ids, id)) return std::nullopt;
    ids.push_back(std::move(id));
    if (comma == std::wstring_view::npos) break;
    start = comma + 1;
  }
  return ids;
}

std::optional<CasePlan> PlanCaseSelection(const std::vector<std::string>& target_cases,
                                          const std::optional<std::vector<std::string>>& requested,
                                          const std::vector<std::string>& skipped,
                                          const std::set<std::string>& baseline_dependents,
                                          std::string* error) {
  const auto reject = [&](std::string reason) -> std::optional<CasePlan> {
    if (error) *error = std::move(reason);
    return std::nullopt;
  };
  for (const auto* ids : {requested ? &*requested : nullptr, &skipped}) {
    if (!ids) continue;
    for (const auto& id : *ids) {
      if (!Contains(target_cases, id)) return reject("unknown case for this target: " + id);
    }
  }

  const std::string baseline(kBaselineCaseId);
  const auto is_selected = [&](const std::string& id) {
    return (!requested || Contains(*requested, id)) && !Contains(skipped, id);
  };
  const bool needs_baseline = std::any_of(
      target_cases.begin(), target_cases.end(),
      [&](const std::string& id) { return is_selected(id) && baseline_dependents.contains(id); });
  const bool add_baseline = needs_baseline && Contains(target_cases, baseline) &&
                            !is_selected(baseline) && !Contains(skipped, baseline);

  CasePlan plan;
  for (const auto& id : target_cases) {
    if (is_selected(id) || (add_baseline && id == baseline)) {
      plan.executed.push_back(id);
    } else {
      plan.excluded.push_back(id);
    }
  }
  if (plan.executed.empty()) return reject("no case is selected for this target");
  if (add_baseline) plan.prerequisites_added.push_back(baseline);
  plan.baseline_case_excluded = needs_baseline && Contains(plan.excluded, baseline);
  return plan;
}

}  // namespace azookey::compat_test
