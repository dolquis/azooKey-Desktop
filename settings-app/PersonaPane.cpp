// C++/WinRT requires the precompiled header first.
// clang-format off
#include "pch.h"
#include "PersonaPane.h"
// clang-format on

#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "LearningPaneModel.h"
#include "ModelPaneModel.h"
#include "SettingsIpcClient.h"
#include "StatusPaneModel.h"
#include "UiDispatch.h"

namespace azookey::settings {
namespace {

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace controls = winrt::Microsoft::UI::Xaml::Controls;
using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

controls::TextBlock Block(const winrt::hstring& text, bool secondary = false) {
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

controls::StackPanel RatioRow(const winrt::hstring& label, double ratio, const wchar_t* id) {
  controls::StackPanel row;
  row.Spacing(4);
  controls::Grid header;
  controls::ColumnDefinition label_column;
  label_column.Width(xaml::GridLengthHelper::FromValueAndType(1, xaml::GridUnitType::Star));
  header.ColumnDefinitions().Append(label_column);
  controls::ColumnDefinition value_column;
  value_column.Width(xaml::GridLengthHelper::Auto());
  header.ColumnDefinitions().Append(value_column);
  header.Children().Append(Block(label));
  auto value = Block(winrt::to_hstring(FormatRatioPercent(ratio)));
  controls::Grid::SetColumn(value, 1);
  SetId(value, std::wstring(id) + L"Value");
  header.Children().Append(value);
  row.Children().Append(header);
  controls::ProgressBar bar;
  bar.Minimum(0);
  bar.Maximum(1);
  bar.Value(std::isfinite(ratio) ? std::clamp(ratio, 0.0, 1.0) : 0.0);
  SetId(bar, std::wstring(id) + L"Bar");
  row.Children().Append(bar);
  return row;
}

}  // namespace

void PersonaPane::Build(controls::StackPanel panel,
                        winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher) {
  dispatcher_ = std::move(dispatcher);
  ResourceLoader resources;

  controls::TextBlock title;
  title.Text(resources.GetString(L"Persona_Title"));
  title.FontSize(20);
  title.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
  panel.Children().Append(title);
  panel.Children().Append(Block(resources.GetString(L"Persona_Description"), true));

  problem_bar_ = controls::InfoBar();
  problem_bar_.IsOpen(false);
  problem_bar_.IsClosable(true);
  problem_bar_.Severity(controls::InfoBarSeverity::Error);
  SetId(problem_bar_, L"PersonaStatusInfoBar");
  panel.Children().Append(problem_bar_);

  refresh_button_ = controls::Button();
  refresh_button_.Content(winrt::box_value(resources.GetString(L"Persona_RefreshButton")));
  SetId(refresh_button_, L"PersonaRefreshButton");
  refresh_button_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (const auto self = weak.lock()) self->Refresh();
  });
  panel.Children().Append(refresh_button_);

  content_ = controls::StackPanel();
  content_.Spacing(12);
  SetId(content_, L"PersonaContent");
  panel.Children().Append(content_);
}

void PersonaPane::OnShown() {
  if (!loaded_once_ && !loading_) Refresh();
}

void PersonaPane::ShowProblem(const winrt::hstring& message) {
  ResourceLoader resources;
  problem_bar_.Title(resources.GetString(L"Persona_ProblemTitle"));
  problem_bar_.Message(message);
  problem_bar_.IsOpen(true);
}

void PersonaPane::RecoverFromFailure() {
  // Reached from a catch block that may run on a background thread, so the flag and controls are
  // only touched from the UI thread.
  dispatcher_.TryEnqueue([self = shared_from_this()] {
    self->loading_ = false;
    self->refresh_button_.IsEnabled(true);
    ResourceLoader resources;
    self->ShowProblem(resources.GetString(L"Persona_UnexpectedError"));
  });
}

winrt::fire_and_forget PersonaPane::Refresh() {
  const auto self = shared_from_this();
  if (loading_) co_return;
  loading_ = true;
  refresh_button_.IsEnabled(false);
  problem_bar_.IsOpen(false);
  const auto dispatcher = dispatcher_;
  try {
    co_await winrt::resume_background();
    const auto result = RequestQueryPersona(DefaultSettingsIpcOptions());
    co_await ResumeForeground(dispatcher);

    loading_ = false;
    refresh_button_.IsEnabled(true);
    ResourceLoader resources;
    if (result.status != HostCallStatus::Ok) {
      ShowProblem(resources.GetString(winrt::to_hstring(HostCallStatusResource(result.status))));
      co_return;
    }
    const auto& persona = *result.response;
    if (!persona.ok) {
      ShowProblem(resources.GetString(
          winrt::to_hstring(LearningErrorResource(persona.error.value_or("")))));
      co_return;
    }
    content_.Children().Clear();
    // With no data the next visit asks again: the Host may have counted commits since.
    loaded_once_ = PersonaHasData(persona);
    if (!PersonaHasData(persona)) {
      content_.Children().Append(Block(resources.GetString(L"Persona_NoData")));
      co_return;
    }
    content_.Children().Append(
        RatioRow(resources.GetString(L"Persona_Polite"), persona.polite_ratio, L"PersonaPolite"));
    content_.Children().Append(
        RatioRow(resources.GetString(L"Persona_Casual"), persona.casual_ratio, L"PersonaCasual"));
    content_.Children().Append(RatioRow(resources.GetString(L"Persona_Technical"),
                                        persona.technical_ratio, L"PersonaTechnical"));
    content_.Children().Append(RatioRow(resources.GetString(L"Persona_Kaomoji"),
                                        persona.kaomoji_ratio, L"PersonaKaomoji"));
    auto basis = Block(resources.GetString(L"Persona_SampleCount") + L": " +
                           winrt::to_hstring(std::to_string(persona.sample_count)) + L"\n" +
                           resources.GetString(L"Persona_ComputedAt") + L": " +
                           winrt::to_hstring(FormatPersonaTime(persona.computed_at_epoch_sec)),
                       true);
    SetId(basis, L"PersonaBasis");
    content_.Children().Append(basis);
  } catch (...) {
    RecoverFromFailure();
  }
}

}  // namespace azookey::settings
