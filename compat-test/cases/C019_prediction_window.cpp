#include <ShellScalingApi.h>
#include <Windows.h>

#include <chrono>
#include <optional>
#include <thread>

#include "runner/CaseSupport.h"
#include "runner/CompatTypes.h"

namespace azookey::compat_test {
namespace {

bool WaitForPrediction(AutomationSession& session) {
  for (int attempt = 0; attempt < 30; ++attempt) {
    if (session.PredictionRect()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

bool WaitForPredictionClose(AutomationSession& session) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!session.PredictionRect()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

bool IsPredictionNearCaret(const RECT& prediction, const RECT& caret) {
  const POINT center{caret.left + (caret.right - caret.left) / 2,
                     caret.top + (caret.bottom - caret.top) / 2};
  const HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;
  const LONG max_gap = MulDiv(64, static_cast<int>(GetDpiForSystem()), USER_DEFAULT_SCREEN_DPI);
  // ComputePlacement clamps the window to the work area when neither side fits.
  const bool near_caret_horizontally =
      prediction.left <= caret.right + max_gap && prediction.right >= caret.left - max_gap;
  const bool overlaps_caret_vertically =
      prediction.top <= caret.bottom && prediction.bottom >= caret.top;
  return IsRectWithinBounds(prediction, info.rcWork) && near_caret_horizontally &&
         overlaps_caret_vertically;
}

std::optional<bool> HasTwoPredictionRows(const RECT& prediction, const RECT& caret) {
  const POINT center{caret.left + (caret.right - caret.left) / 2,
                     caret.top + (caret.bottom - caret.top) / 2};
  const HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
  UINT dpi_x = 0;
  UINT dpi_y = 0;
  if (!monitor || FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) || !dpi_x)
    return std::nullopt;
  // PredictionWindow uses 8 px padding and 28 px per visible row at 96 DPI.
  const int height_for_two_rows = 2 * MulDiv(8, dpi_x, USER_DEFAULT_SCREEN_DPI) +
                                  2 * MulDiv(28, dpi_x, USER_DEFAULT_SCREEN_DPI);
  return prediction.bottom - prediction.top >= height_for_two_rows;
}

}  // namespace

CaseDefinition MakeC019PredictionWindowCase() {
  return {
      "C-019",
      [](AutomationSession& session) {
        CaseResult result;
        result.id = "C-019";
        result.status = ResultStatus::FailingSkip;
        if (!session.baseline_verified()) {
          result.reason_code = "baseline-conversion-not-verified";
          return result;
        }
        if (!session.ClearEditor() || !session.SendAscii("niho")) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        if (!WaitForPrediction(session)) {
          result.reason_code = "prediction-window-unavailable";
          return result;
        }
        const auto prediction = session.PredictionRect();
        const auto caret = session.CaretRect();
        if (!prediction || !caret) {
          result.reason_code = "prediction-or-caret-rectangle-unavailable";
          return result;
        }
        if (!IsPredictionNearCaret(*prediction, *caret)) {
          result.status = ResultStatus::Fail;
          result.reason_code = "prediction-position-out-of-range";
          return result;
        }
        const auto before_tab = session.ReadEditorText();
        if (!before_tab || *before_tab != L"にほ") {
          result.reason_code = "prediction-preedit-unobservable";
          return result;
        }
        if (!session.SendVirtualKey(VK_TAB)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        auto after_tab = session.ReadEditorText();
        for (int attempt = 0; after_tab && *after_tab == *before_tab && attempt < 20; ++attempt) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          after_tab = session.ReadEditorText();
        }
        if (!after_tab) {
          result.reason_code = "prediction-tab-result-unobservable";
          return result;
        }
        if (after_tab->size() <= before_tab->size() ||
            after_tab->compare(0, before_tab->size(), *before_tab) != 0 ||
            after_tab->find(L'\t') != std::wstring::npos) {
          result.status = ResultStatus::Fail;
          result.reason_code = "prediction-tab-not-accepted";
          return result;
        }
        if (!session.ClearEditor() || !session.SendAscii("niho")) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        if (!WaitForPrediction(session)) {
          result.reason_code = "prediction-window-unavailable-for-shift-tab";
          return result;
        }
        const auto shift_tab_prediction = session.PredictionRect();
        const auto shift_tab_caret = session.CaretRect();
        if (!shift_tab_prediction || !shift_tab_caret) {
          result.reason_code = "prediction-row-count-unobservable";
          return result;
        }
        const auto has_two_rows = HasTwoPredictionRows(*shift_tab_prediction, *shift_tab_caret);
        if (!has_two_rows) {
          result.reason_code = "prediction-row-count-unobservable";
          return result;
        }
        if (!*has_two_rows) {
          result.reason_code = "prediction-second-candidate-unavailable";
          return result;
        }
        if (!session.SendModifiedKey({VK_SHIFT}, VK_TAB)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        auto after_shift_tab = session.ReadEditorText();
        for (int attempt = 0; after_shift_tab && *after_shift_tab == L"にほ" && attempt < 20;
             ++attempt) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          after_shift_tab = session.ReadEditorText();
        }
        if (!after_shift_tab) {
          result.reason_code = "prediction-shift-tab-result-unobservable";
          return result;
        }
        if (after_shift_tab->size() <= 2 || after_shift_tab->compare(0, 2, L"にほ") != 0 ||
            after_shift_tab->find(L'\t') != std::wstring::npos) {
          result.status = ResultStatus::Fail;
          result.reason_code = "prediction-shift-tab-not-accepted";
          return result;
        }
        if (!session.ClearEditor() || !session.SendAscii("niho")) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        if (!WaitForPrediction(session)) {
          result.reason_code = "prediction-window-unavailable-for-escape";
          return result;
        }
        const auto before_escape = session.ReadEditorText();
        if (!before_escape || before_escape->empty()) {
          result.reason_code = "prediction-escape-preedit-unobservable";
          return result;
        }
        if (!session.SendVirtualKey(VK_ESCAPE)) {
          result.reason_code = session.input_failure_reason();
          return result;
        }
        if (!WaitForPredictionClose(session)) {
          result.status = ResultStatus::Fail;
          result.reason_code = "prediction-escape-did-not-close-window";
          return result;
        }
        const auto after_escape = session.ReadEditorText();
        if (!after_escape) {
          result.reason_code = "prediction-escape-result-unobservable";
        } else if (*after_escape != *before_escape) {
          result.status = ResultStatus::Fail;
          result.reason_code = "prediction-escape-changed-preedit";
        } else {
          result.status = ResultStatus::Pass;
          result.reason_code = "prediction-window-tab-shift-tab-and-escape-observed";
        }
        return result;
      },
  };
}

}  // namespace azookey::compat_test
