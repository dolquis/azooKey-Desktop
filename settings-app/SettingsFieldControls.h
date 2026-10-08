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
  void Apply(const SettingValues& values);
  // Every field's value; returns false with the path of a control that holds no valid value.
  bool Read(SettingValues* values, std::string* invalid_path) const;

 private:
  struct Entry {
    const SettingField* field;
    winrt::Microsoft::UI::Xaml::Controls::Control control{nullptr};
  };

  void RefreshActiveStates();
  std::optional<SettingValue> ReadEntry(const Entry& entry) const;

  std::vector<Entry> entries_;
};

}  // namespace azookey::settings
