// C++/WinRT requires the precompiled header first.
// clang-format off
#include "pch.h"
#include "ProofreadPane.h"
// clang-format on

#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <string>

#include "ModelPaneModel.h"
#include "ProofreadModel.h"
#include "SettingsIpcClient.h"
#include "StatusPaneModel.h"
#include "UiDispatch.h"

namespace azookey::settings {
namespace {

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace controls = winrt::Microsoft::UI::Xaml::Controls;
using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

// A TextBlock shows its Text as plain text: no markup, links or commands are interpreted.
controls::TextBlock PlainText(const winrt::hstring& text, bool secondary = false) {
  controls::TextBlock block;
  block.Text(text);
  block.TextWrapping(xaml::TextWrapping::Wrap);
  block.IsTextSelectionEnabled(true);
  if (secondary) {
    block.FontSize(12);
    block.Opacity(0.8);
  }
  return block;
}

void SetId(xaml::DependencyObject const& object, const std::wstring& id) {
  xaml::Automation::AutomationProperties::SetAutomationId(object, id);
}

}  // namespace

void ProofreadPane::Build(controls::StackPanel panel,
                          winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher) {
  dispatcher_ = std::move(dispatcher);
  ResourceLoader resources;

  controls::TextBlock title;
  title.Text(resources.GetString(L"Proofread_Title"));
  title.FontSize(20);
  title.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
  panel.Children().Append(title);
  panel.Children().Append(PlainText(resources.GetString(L"Proofread_Description"), true));

  // Always visible, so it is on screen before anything is sent.
  controls::InfoBar notice;
  notice.Severity(controls::InfoBarSeverity::Warning);
  notice.IsOpen(true);
  notice.IsClosable(false);
  notice.Title(resources.GetString(L"Proofread_ExternalNoticeTitle"));
  notice.Message(resources.GetString(L"Proofread_ExternalNoticeMessage"));
  SetId(notice, L"ProofreadExternalNotice");
  panel.Children().Append(notice);

  input_ = controls::TextBox();
  input_.AcceptsReturn(true);
  input_.TextWrapping(xaml::TextWrapping::Wrap);
  input_.MinHeight(160);
  input_.MaxHeight(360);
  input_.Header(winrt::box_value(resources.GetString(L"Proofread_InputHeader")));
  input_.PlaceholderText(resources.GetString(L"Proofread_InputPlaceholder"));
  SetId(input_, L"ProofreadInput");
  input_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (const auto self = weak.lock()) self->UpdateCounter();
  });
  panel.Children().Append(input_);

  counter_ = PlainText(L"", true);
  SetId(counter_, L"ProofreadCounter");
  panel.Children().Append(counter_);

  check_button_ = controls::Button();
  check_button_.Content(winrt::box_value(resources.GetString(L"Proofread_CheckButton")));
  SetId(check_button_, L"ProofreadCheckButton");
  check_button_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (const auto self = weak.lock()) self->Check();
  });
  panel.Children().Append(check_button_);

  status_bar_ = controls::InfoBar();
  status_bar_.IsOpen(false);
  status_bar_.IsClosable(true);
  SetId(status_bar_, L"ProofreadStatusInfoBar");
  panel.Children().Append(status_bar_);

  results_ = controls::StackPanel();
  results_.Spacing(10);
  SetId(results_, L"ProofreadResults");
  panel.Children().Append(results_);
  UpdateCounter();
}

void ProofreadPane::UpdateCounter() {
  ResourceLoader resources;
  const auto bytes = winrt::to_string(input_.Text()).size();
  const bool too_large = bytes > azookey::ipc::kMaxAnomalyTextBytes;
  counter_.Text(winrt::to_hstring(std::to_string(bytes) + " / " +
                                  std::to_string(azookey::ipc::kMaxAnomalyTextBytes) + " bytes") +
                (too_large ? resources.GetString(L"Proofread_TooLargeSuffix") : winrt::hstring()));
}

void ProofreadPane::ShowStatus(controls::InfoBarSeverity severity, const winrt::hstring& message) {
  ResourceLoader resources;
  status_bar_.Severity(severity);
  status_bar_.Title(resources.GetString(severity == controls::InfoBarSeverity::Success
                                            ? L"Proofread_DoneTitle"
                                            : L"Proofread_ProblemTitle"));
  status_bar_.Message(message);
  status_bar_.IsOpen(true);
}

void ProofreadPane::RecoverFromFailure() {
  // Reached from a catch block that may run on a background thread, so the flag and controls are
  // only touched from the UI thread.
  dispatcher_.TryEnqueue([self = shared_from_this()] {
    self->busy_ = false;
    self->check_button_.IsEnabled(true);
    ResourceLoader resources;
    self->ShowStatus(controls::InfoBarSeverity::Error,
                     resources.GetString(L"Proofread_UnexpectedError"));
  });
}

winrt::fire_and_forget ProofreadPane::Check() {
  const auto self = shared_from_this();
  if (busy_) co_return;
  ResourceLoader resources;
  status_bar_.IsOpen(false);
  const auto text = input_.Text();
  const auto utf8 = winrt::to_string(text);
  switch (CheckProofreadInput(utf8)) {
    case ProofreadInputProblem::Empty:
      ShowStatus(controls::InfoBarSeverity::Warning, resources.GetString(L"Proofread_EmptyInput"));
      co_return;
    case ProofreadInputProblem::TooLarge:
      // Over the limit: nothing is sent.
      ShowStatus(controls::InfoBarSeverity::Warning, resources.GetString(L"Proofread_TooLarge"));
      co_return;
    case ProofreadInputProblem::None:
      break;
  }

  busy_ = true;
  check_button_.IsEnabled(false);
  results_.Children().Clear();
  results_.Children().Append(PlainText(resources.GetString(L"Proofread_Checking"), true));
  const auto request = MakeProofreadRequest(utf8);
  const auto dispatcher = dispatcher_;
  try {
    co_await winrt::resume_background();
    const auto result = RequestDetectAnomalies(DefaultSettingsIpcOptions(), request);
    co_await ResumeForeground(dispatcher);

    busy_ = false;
    check_button_.IsEnabled(true);
    results_.Children().Clear();
    ResourceLoader after;
    if (result.status != HostCallStatus::Ok) {
      ShowStatus(controls::InfoBarSeverity::Error,
                 after.GetString(winrt::to_hstring(HostCallStatusResource(result.status))));
      co_return;
    }
    const auto& response = *result.response;
    if (!response.ok) {
      ShowStatus(
          controls::InfoBarSeverity::Error,
          after.GetString(winrt::to_hstring(ProofreadErrorResource(response.error.value_or("")))));
      co_return;
    }
    checked_text_ = std::wstring(text);
    if (response.findings.empty()) {
      results_.Children().Append(PlainText(after.GetString(L"Proofread_NoFindings")));
      co_return;
    }
    int number = 0;
    for (const auto& finding : response.findings) {
      ++number;
      controls::StackPanel card;
      card.Spacing(4);
      const auto excerpt = FindingExcerpt(checked_text_, finding.start, finding.length);
      auto heading = PlainText(
          winrt::to_hstring(std::to_string(number)) + L". " +
          (excerpt ? winrt::hstring(*excerpt) : after.GetString(L"Proofread_RangeUnavailable")));
      heading.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
      card.Children().Append(heading);
      card.Children().Append(PlainText(winrt::to_hstring(finding.reason)));
      for (const auto& suggestion : finding.suggestions) {
        card.Children().Append(PlainText(after.GetString(L"Proofread_SuggestionPrefix") +
                                         winrt::to_hstring(suggestion)));
      }
      card.Children().Append(
          PlainText(after.GetString(L"Proofread_Confidence") + L": " +
                        winrt::to_hstring(FormatRatioPercent(finding.confidence)),
                    true));
      if (excerpt) {
        controls::Button select;
        select.Content(winrt::box_value(after.GetString(L"Proofread_SelectButton")));
        SetId(select, L"ProofreadSelectButton_" + std::to_wstring(number));
        select.Click([weak = weak_from_this(), start = finding.start, length = finding.length](
                         auto const&, auto const&) {
          const auto pane = weak.lock();
          if (!pane) return;
          // Ranges can overlap; selecting one never changes the others. The text must still be the
          // one that was checked, or the offsets point at something else.
          if (std::wstring(pane->input_.Text()) != pane->checked_text_) return;
          pane->input_.Focus(xaml::FocusState::Programmatic);
          pane->input_.Select(static_cast<int32_t>(start), static_cast<int32_t>(length));
        });
        card.Children().Append(select);
      }
      results_.Children().Append(card);
    }
  } catch (...) {
    RecoverFromFailure();
  }
}

}  // namespace azookey::settings
