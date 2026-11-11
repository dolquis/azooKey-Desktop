#pragma once

#include <functional>
#include <string>
#include <vector>

#include "SettingsFields.h"
#include "pch.h"

namespace azookey::settings {

// Builds the controls of the generic panes from GenericSettingFields() and moves values between
// them and SettingValues. Each control's AutomationId is the field's settings.json path.
class SettingsFieldControls {
 public:
  using PanelForPane =
      std::function<winrt::Microsoft::UI::Xaml::Controls::StackPanel(SettingsPane)>;

  void Build(const PanelForPane& panels);
  // Runs after any control's value changes (may be empty).
  void SetChangedHandler(std::function<void()> changed) { changed_ = std::move(changed); }
  void Apply(const SettingValues& values);
  // Every valid field value; returns false with the path of an enabled control that holds no
  // valid value. Disabled controls with no valid value are left out.
  bool Read(SettingValues* values, std::string* invalid_path) const;
  // Every valid field value, and the paths of enabled controls that hold no valid value.
  SettingValues ReadValid(std::vector<std::string>* invalid_paths) const;
  // Moves keyboard focus to the field's control (its pane must already be shown), opening the
  // details Expander the control is folded in.
  void Focus(std::string_view path) const;
  // Whether the condition holds for the values the controls show now (not the saved ones).
  bool ConditionHolds(const SettingCondition& condition) const;
  // "「{0}」が「{1}」のときに使えます。" for the condition, for screen readers.
  winrt::hstring ConditionText(const SettingCondition& condition) const;

 private:
  struct Entry {
    const SettingField* field;
    winrt::Microsoft::UI::Xaml::Controls::Control control{nullptr};
    // The details Expander the control is folded in, or null.
    winrt::Microsoft::UI::Xaml::Controls::Expander details{nullptr};
    winrt::hstring note;
    winrt::hstring inactive_reason;
  };

  void RefreshActiveStates();
  SettingValues CurrentValues() const;
  std::optional<SettingValue> ReadEntry(const Entry& entry) const;

  std::vector<Entry> entries_;
  std::function<void()> changed_;
};

}  // namespace azookey::settings
