// C++/WinRT requires the precompiled header first.
// clang-format off
#include "pch.h"
#include "SettingsFieldControls.h"
// clang-format on

#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.NumberFormatting.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace azookey::settings {
namespace {

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace controls = winrt::Microsoft::UI::Xaml::Controls;
using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

winrt::hstring Resource(const ResourceLoader& resources, const std::string& name) {
  return resources.GetString(winrt::to_hstring(name));
}

controls::TextBlock SectionHeading(const winrt::hstring& text) {
  controls::TextBlock heading;
  heading.Text(text);
  heading.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
  heading.Margin(xaml::ThicknessHelper::FromLengths(0, 8, 0, 0));
  return heading;
}

controls::TextBlock Note(const winrt::hstring& text) {
  controls::TextBlock note;
  note.Text(text);
  note.TextWrapping(xaml::TextWrapping::Wrap);
  note.FontSize(12);
  note.Opacity(0.8);
  return note;
}

controls::NumberBox NumberControl(const SettingField& field) {
  const bool integer = field.kind == SettingKind::Integer;
  controls::NumberBox box;
  box.SpinButtonPlacementMode(controls::NumberBoxSpinButtonPlacementMode::Compact);
  box.SmallChange(integer ? 1.0 : 0.05);
  if (field.minimum) box.Minimum(*field.minimum);
  if (field.maximum) box.Maximum(*field.maximum);
  winrt::Windows::Globalization::NumberFormatting::IncrementNumberRounder rounder;
  rounder.Increment(integer ? 1.0 : 0.01);
  rounder.RoundingAlgorithm(
      winrt::Windows::Globalization::NumberFormatting::RoundingAlgorithm::RoundHalfUp);
  winrt::Windows::Globalization::NumberFormatting::DecimalFormatter formatter;
  formatter.IntegerDigits(1);
  formatter.FractionDigits(integer ? 0 : 2);
  formatter.IsGrouped(false);
  formatter.NumberRounder(rounder);
  box.NumberFormatter(formatter);
  return box;
}

}  // namespace

void SettingsFieldControls::Build(const PanelForPane& panels) {
  ResourceLoader resources;
  std::string_view section;
  std::optional<SettingsPane> pane;
  for (const auto& field : GenericSettingFields()) {
    auto panel = panels(field.pane);
    if (pane != field.pane || section != field.section) {
      panel.Children().Append(
          SectionHeading(Resource(resources, SettingSectionResource(field.section))));
      pane = field.pane;
      section = field.section;
    }
    const auto label = winrt::box_value(Resource(resources, SettingLabelResource(field.path)));
    controls::Control control{nullptr};
    switch (field.kind) {
      case SettingKind::Bool: {
        controls::ToggleSwitch toggle;
        toggle.Header(label);
        toggle.Toggled([this](auto&&, auto&&) { RefreshActiveStates(); });
        control = toggle;
        break;
      }
      case SettingKind::Enum: {
        controls::ComboBox combo;
        combo.Header(label);
        combo.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
        for (const auto option : field.options) {
          controls::ComboBoxItem item;
          item.Content(
              winrt::box_value(Resource(resources, SettingOptionResource(field.path, option))));
          item.Tag(winrt::box_value(winrt::to_hstring(option)));
          combo.Items().Append(item);
        }
        combo.SelectionChanged([this](auto&&, auto&&) { RefreshActiveStates(); });
        control = combo;
        break;
      }
      case SettingKind::Integer:
      case SettingKind::Number: {
        auto box = NumberControl(field);
        box.Header(label);
        control = box;
        break;
      }
      case SettingKind::String:
      case SettingKind::StringList: {
        controls::TextBox text;
        text.Header(label);
        if (field.kind == SettingKind::StringList) {
          text.AcceptsReturn(true);
          text.TextWrapping(xaml::TextWrapping::Wrap);
          text.MinHeight(80);
        }
        control = text;
        break;
      }
    }
    xaml::Automation::AutomationProperties::SetAutomationId(control, winrt::to_hstring(field.path));
    panel.Children().Append(control);
    if (field.described) {
      panel.Children().Append(Note(Resource(resources, SettingDescriptionResource(field.path))));
    }
    entries_.push_back({&field, control});
  }
}

void SettingsFieldControls::Apply(const SettingValues& values) {
  for (const auto& entry : entries_) {
    const auto value = SettingValueOrDefault(*entry.field, values);
    switch (entry.field->kind) {
      case SettingKind::Bool:
        entry.control.as<controls::ToggleSwitch>().IsOn(std::get<bool>(value));
        break;
      case SettingKind::Enum: {
        const auto& options = entry.field->options;
        const auto selected =
            std::find(options.begin(), options.end(), std::get<std::string>(value));
        entry.control.as<controls::ComboBox>().SelectedIndex(
            static_cast<int32_t>(selected - options.begin()));
        break;
      }
      case SettingKind::Integer:
        entry.control.as<controls::NumberBox>().Value(
            static_cast<double>(std::get<int64_t>(value)));
        break;
      case SettingKind::Number:
        entry.control.as<controls::NumberBox>().Value(std::get<double>(value));
        break;
      case SettingKind::String:
        entry.control.as<controls::TextBox>().Text(winrt::to_hstring(std::get<std::string>(value)));
        break;
      case SettingKind::StringList:
        entry.control.as<controls::TextBox>().Text(
            winrt::to_hstring(FormatSettingList(std::get<std::vector<std::string>>(value))));
        break;
    }
  }
  RefreshActiveStates();
}

std::optional<SettingValue> SettingsFieldControls::ReadEntry(const Entry& entry) const {
  switch (entry.field->kind) {
    case SettingKind::Bool:
      return entry.control.as<controls::ToggleSwitch>().IsOn();
    case SettingKind::Enum: {
      const auto item = entry.control.as<controls::ComboBox>().SelectedItem();
      if (!item) return std::nullopt;
      return winrt::to_string(
          winrt::unbox_value<winrt::hstring>(item.as<controls::ComboBoxItem>().Tag()));
    }
    case SettingKind::Integer: {
      // An emptied NumberBox holds NaN.
      const double number = entry.control.as<controls::NumberBox>().Value();
      if (!std::isfinite(number) || std::floor(number) != number) return std::nullopt;
      return static_cast<int64_t>(number);
    }
    case SettingKind::Number: {
      const double number = entry.control.as<controls::NumberBox>().Value();
      if (!std::isfinite(number)) return std::nullopt;
      return number;
    }
    case SettingKind::String:
      return winrt::to_string(entry.control.as<controls::TextBox>().Text());
    case SettingKind::StringList:
      return ParseSettingList(winrt::to_string(entry.control.as<controls::TextBox>().Text()));
  }
  return std::nullopt;
}

bool SettingsFieldControls::Read(SettingValues* values, std::string* invalid_path) const {
  for (const auto& entry : entries_) {
    auto value = ReadEntry(entry);
    if (!value || ValidateSettingValue(*entry.field, *value)) {
      *invalid_path = std::string(entry.field->path);
      return false;
    }
    (*values)[std::string(entry.field->path)] = std::move(*value);
  }
  return true;
}

void SettingsFieldControls::RefreshActiveStates() {
  SettingValues current;
  for (const auto& entry : entries_) {
    if (auto value = ReadEntry(entry)) current[std::string(entry.field->path)] = std::move(*value);
  }
  for (const auto& entry : entries_) {
    entry.control.IsEnabled(IsSettingFieldActive(*entry.field, current));
  }
}

}  // namespace azookey::settings
