#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace azookey::settings {

// A settings.json value edited through the generic panes (sideload-packaging-spec section 3.6).
using SettingValue = std::variant<bool, int64_t, double, std::string, std::vector<std::string>>;

// Keyed by the dotted path into settings.json, for example "privacy.custom.learning".
using SettingValues = std::map<std::string, SettingValue, std::less<>>;

enum class SettingKind { Bool, Integer, Number, Enum, String, StringList };

enum class SettingsPane { General, Input, Dictionary, Ai, Privacy, Advanced };

// The field is editable only while another field holds this value (the feature spec says it
// has no effect otherwise). `equals` is "true"/"false" for a bool or an enum value.
struct SettingCondition {
  std::string_view path;
  std::string_view equals;
};

struct SettingField {
  std::string_view path;
  SettingKind kind;
  SettingsPane pane;
  // Resource name suffix of the sub-heading the field is listed under.
  std::string_view section;
  SettingValue default_value;
  std::optional<double> minimum;
  std::optional<double> maximum;
  std::vector<std::string_view> options;
  std::optional<SettingCondition> active_when;
  // Whether a note (SettingDescriptionResource) is shown under the control.
  bool described{false};
};

// A schema key the settings app does not edit, and why (sideload-packaging-spec section 3.6).
struct UneditedSetting {
  std::string_view path;
  std::string_view reason;
};

// Fields shown by the generic panes, in display order. Defaults, ranges and enum values match
// settings/mvp-settings.schema.json (checked by azookey_settings_persistence_tests).
const std::vector<SettingField>& GenericSettingFields();

// Schema keys edited by dedicated controls (EditableSettings' typed members).
const std::vector<std::string_view>& DedicatedSettingPaths();

// Schema keys kept on save without an editing control.
const std::vector<UneditedSetting>& UneditedSettings();

const SettingField* FindSettingField(std::string_view path);

// The stored value if it is valid for the field, otherwise the schema default.
SettingValue SettingValueOrDefault(const SettingField& field, const SettingValues& values);

// Empty when the value has the field's type and lies within its range and options.
std::optional<std::string> ValidateSettingValue(const SettingField& field,
                                                const SettingValue& value);

bool IsSettingFieldActive(const SettingField& field, const SettingValues& values);

// One entry per line; surrounding blanks are trimmed and empty lines dropped.
std::vector<std::string> ParseSettingList(std::string_view text);
std::string FormatSettingList(const std::vector<std::string>& items);

// Resource names in Strings/ja-JP/Resources.resw for generated controls.
std::string SettingLabelResource(std::string_view path);
std::string SettingDescriptionResource(std::string_view path);
std::string SettingOptionResource(std::string_view path, std::string_view option);
std::string SettingSectionResource(std::string_view section);

}  // namespace azookey::settings
