#pragma once

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::compat_test {

// C-002〜C-010 と C-012 は、C-001 の変換成功で azooKey の基準動作を確認できた場合だけ実行する。
inline constexpr std::string_view kBaselineCaseId = "C-001";

struct CasePlan {
  // target の cases の順に並べる。C-010 を最後に置く target の順序を選択後も保つ。
  std::vector<std::string> executed;
  std::vector<std::string> excluded;
  // --cases で依存ケースだけを選んだときに、前提として追加した C-001。
  std::vector<std::string> prerequisites_added;
  // --skip で C-001 を除外したまま依存ケースを実行する場合。依存ケースは実行せず
  // baseline-case-excluded の failing-skip にする。
  bool baseline_case_excluded{false};
};

// "C-001,C-004" 形式のカンマ区切りを解析する。空要素、C-NNN 以外の形式、重複は拒否する。
std::optional<std::vector<std::string>> ParseCaseIdList(std::wstring_view text);

// requested が無ければ target の全ケースを選び、skipped を除く。target に無い ID は
// error に理由を入れて std::nullopt を返す。選択が空になる場合も拒否する。
std::optional<CasePlan> PlanCaseSelection(const std::vector<std::string>& target_cases,
                                          const std::optional<std::vector<std::string>>& requested,
                                          const std::vector<std::string>& skipped,
                                          const std::set<std::string>& baseline_dependents,
                                          std::string* error);

}  // namespace azookey::compat_test
