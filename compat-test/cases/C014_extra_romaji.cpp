#include <Windows.h>

#include <array>
#include <string>
#include <string_view>

#include "runner/CompatTypes.h"

namespace azookey::compat_test {

CaseDefinition MakeC014ExtraRomajiCase() {
  return {
      "C-014",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-014";
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        struct Sample {
          std::string_view input;
          std::wstring_view expected;
        };
        constexpr std::array<Sample, 4> kSamples{{
            {"kitto", L"きっと"},
            {"syatu", L"しゃつ"},
            {"siro", L"しろ"},
            {"nn", L"ん"},
        }};
        for (const auto& sample : kSamples) {
          if (!session.ClearEditor() || !session.SendAscii(std::string(sample.input))) {
            result.reason_code = session.input_failure_reason();
            return result;
          }
          const auto preedit = session.ReadEditorText();
          if (!preedit) {
            result.reason_code = "preedit-text-unobservable";
            return result;
          }
          if (*preedit != sample.expected) {
            result.status = ResultStatus::Fail;
            result.reason_code = "extra-romaji-preedit-mismatch";
            return result;
          }
          if (!session.SendVirtualKey(VK_RETURN)) {
            result.reason_code = session.input_failure_reason();
            return result;
          }
          const auto committed = session.ReadEditorText();
          if (!committed) {
            result.reason_code = "committed-text-unobservable";
            return result;
          }
          if (*committed != sample.expected) {
            result.status = ResultStatus::Fail;
            result.reason_code = "extra-romaji-commit-mismatch";
            return result;
          }
        }
        result.status = ResultStatus::Pass;
        result.reason_code = "extra-romaji-preedit-and-commit-observed";
        return result;
      },
  };
}

}  // namespace azookey::compat_test
