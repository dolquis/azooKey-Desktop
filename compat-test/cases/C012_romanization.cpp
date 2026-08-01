#include <array>
#include <string_view>

#include "runner/CompatTypes.h"

namespace azookey::compat_test {

CaseDefinition MakeC012RomanizationCase() {
  return {
      "C-012",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-012";
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        struct Sample {
          std::string_view input;
          std::wstring_view expected;
        };
        constexpr std::array<Sample, 6> kSamples{{
            {"ja", L"じゃ"},
            {"ju", L"じゅ"},
            {"jo", L"じょ"},
            {"jya", L"じゃ"},
            {"jyu", L"じゅ"},
            {"jyo", L"じょ"},
        }};
        for (const auto& sample : kSamples) {
          if (!session.ClearEditor() || !session.SendAscii(std::string(sample.input))) {
            result.reason_code = session.input_failure_reason();
            return result;
          }
          const auto preedit = session.ReadEditorText();
          if (!preedit || preedit->find(sample.expected) == std::wstring::npos) {
            result.status = ResultStatus::Fail;
            result.reason_code = "romanization-preedit-mismatch";
            return result;
          }
          if (!session.SendVirtualKey(VK_RETURN)) {
            result.reason_code = session.input_failure_reason();
            return result;
          }
          const auto committed = session.ReadEditorText();
          if (!committed || committed->find(sample.expected) == std::wstring::npos) {
            result.status = ResultStatus::Fail;
            result.reason_code = "romanization-commit-mismatch";
            return result;
          }
        }
        result.status = ResultStatus::Pass;
        result.reason_code = "romanization-preedit-and-commit-observed";
        return result;
      },
  };
}

}  // namespace azookey::compat_test
