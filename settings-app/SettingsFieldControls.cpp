// C++/WinRT requires the precompiled header first.
// clang-format off
#include "pch.h"
#include "SettingsFieldControls.h"
// clang-format on

#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.NumberFormatting.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

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
  xaml::Automation::AutomationProperties::SetHeadingLevel(
      heading, xaml::Automation::Peers::AutomationHeadingLevel::Level3);
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
  // The bounds limit the spin buttons only. Typed values are neither clamped nor rounded, so an
  // out-of-range value is refused at save and a stored value is shown as it is (section 3.7).
  box.ValidationMode(controls::NumberBoxValidationMode::Disabled);
  if (field.minimum) box.Minimum(*field.minimum);
  if (field.maximum) box.Maximum(*field.maximum);
  winrt::Windows::Globalization::NumberFormatting::DecimalFormatter formatter;
  formatter.IntegerDigits(1);
  formatter.FractionDigits(0);
  formatter.IsGrouped(false);
  box.NumberFormatter(formatter);
  return box;
}

// Names the switch a disabled control waits for, for screen readers.
winrt::hstring InactiveReason(const ResourceLoader& resources, const SettingCondition& condition) {
  const auto* parent = FindSettingField(condition.path);
  if (!parent) return {};
  std::wstring text(resources.GetString(parent->kind == SettingKind::Bool
                                            ? L"SettingInactiveUntilOn"
                                            : L"SettingInactiveUntilValue"));
  const auto replace = [&text](std::wstring_view marker, const winrt::hstring& value) {
    if (const auto at = text.find(marker); at != std::wstring::npos) {
      text.replace(at, marker.size(), value);
    }
  };
  replace(L"{0}", Resource(resources, SettingLabelResource(parent->path)));
  if (parent->kind != SettingKind::Bool) {
    replace(L"{1}", Resource(resources, SettingOptionResource(parent->path, condition.equals)));
  }
  return winrt::hstring(text);
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
    Entry entry{&field, control};
    if (field.described) {
      entry.note = Resource(resources, SettingDescriptionResource(field.path));
      panel.Children().Append(Note(entry.note));
    }
    if (field.active_when) entry.inactive_reason = InactiveReason(resources, *field.active_when);
    entries_.push_back(std::move(entry));
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
  SettingValues read;
  std::vector<const SettingField*> invalid;
  for (const auto& entry : entries_) {
    auto value = ReadEntry(entry);
    if (value && !ValidateSettingValue(*entry.field, *value)) {
      read[std::string(entry.field->path)] = std::move(*value);
    } else {
      invalid.push_back(entry.field);
    }
  }
  // A disabled control cannot be corrected, so its invalid value is left out of the save.
  for (const auto* field : invalid) {
    if (IsSettingFieldActive(*field, read)) {
      *invalid_path = std::string(field->path);
      return false;
    }
  }
  for (auto& [path, value] : read) (*values)[path] = std::move(value);
  return true;
}

void SettingsFieldControls::RefreshActiveStates() {
  SettingValues current;
  for (const auto& entry : entries_) {
    if (auto value = ReadEntry(entry)) current[std::string(entry.field->path)] = std::move(*value);
  }
  for (const auto& entry : entries_) {
    const bool active = IsSettingFieldActive(*entry.field, current);
    entry.control.IsEnabled(active);
    auto help = entry.note;
    if (!active && !entry.inactive_reason.empty()) {
      help = help.empty() ? entry.inactive_reason : help + L" " + entry.inactive_reason;
    }
    xaml::Automation::AutomationProperties::SetHelpText(entry.control, help);
  }
}

void SettingsFieldControls::Focus(std::string_view path) const {
  for (const auto& entry : entries_) {
    if (entry.field->path != path) continue;
    entry.control.StartBringIntoView();
    entry.control.Focus(xaml::FocusState::Programmatic);
    return;
  }
}

}  // namespace azookey::settings
