#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "SettingsFields.h"
#include "azookey/ipc/Json.h"

namespace {

namespace j = azookey::ipc::json;
using azookey::settings::SettingKind;

std::string ReadFile(const char8_t* path) {
  std::ifstream input(std::filesystem::path(std::u8string(path)), std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

const j::Value& Schema() {
  static const j::Value schema = *j::Parse(ReadFile(u8"" AZOOKEY_SETTINGS_SCHEMA_PATH));
  return schema;
}

const j::Value* SchemaNode(std::string_view path) {
  const j::Value* node = &Schema();
  while (!path.empty()) {
    const auto dot = path.find('.');
    const auto* properties = node->Find("properties");
    if (!properties) return nullptr;
    node = properties->Find(path.substr(0, dot));
    if (!node) return nullptr;
    path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
  }
  return node;
}

// Leaves are scalars, arrays and map-shaped objects; fixed objects are walked into.
void CollectLeaves(const j::Value& node, const std::string& path, std::set<std::string>* leaves) {
  if (const auto* properties = node.Find("properties"); properties && properties->IsObject()) {
    for (const auto& [key, child] : properties->AsObject()) {
      CollectLeaves(child, path.empty() ? key : path + "." + key, leaves);
    }
    return;
  }
  leaves->insert(path);
}

std::set<std::string> ResourceNames() {
  const auto resw = ReadFile(u8"" AZOOKEY_SETTINGS_RESW_PATH);
  std::set<std::string> names;
  const std::string marker = "<data name=\"";
  for (auto at = resw.find(marker); at != std::string::npos; at = resw.find(marker, at)) {
    at += marker.size();
    names.insert(resw.substr(at, resw.find('"', at) - at));
  }
  return names;
}

}  // namespace

TEST(SettingsFieldsTest, GenericFieldsMatchTheSchema) {
  for (const auto& field : azookey::settings::GenericSettingFields()) {
    SCOPED_TRACE(std::string(field.path));
    const auto* node = SchemaNode(field.path);
    ASSERT_NE(node, nullptr);
    const auto type = node->GetString("type").value_or("");
    const auto* fallback = node->Find("default");
    ASSERT_NE(fallback, nullptr);
    switch (field.kind) {
      case SettingKind::Bool:
        EXPECT_EQ(type, "boolean");
        EXPECT_EQ(std::get<bool>(field.default_value), fallback->AsBool());
        break;
      case SettingKind::Integer:
        EXPECT_EQ(type, "integer");
        EXPECT_EQ(static_cast<double>(std::get<int64_t>(field.default_value)),
                  fallback->AsNumber());
        break;
      case SettingKind::Number:
        EXPECT_EQ(type, "number");
        EXPECT_EQ(std::get<double>(field.default_value), fallback->AsNumber());
        break;
      case SettingKind::Enum: {
        EXPECT_EQ(type, "string");
        EXPECT_EQ(std::get<std::string>(field.default_value), fallback->AsString());
        const auto* options = node->GetArray("enum");
        ASSERT_NE(options, nullptr);
        std::vector<std::string> expected;
        for (const auto& option : *options) expected.push_back(option.AsString());
        EXPECT_EQ(std::vector<std::string>(field.options.begin(), field.options.end()), expected);
        break;
      }
      case SettingKind::String:
        EXPECT_EQ(type, "string");
        EXPECT_EQ(node->Find("enum"), nullptr);
        EXPECT_EQ(std::get<std::string>(field.default_value), fallback->AsString());
        break;
      case SettingKind::StringList:
        EXPECT_EQ(type, "array");
        EXPECT_EQ(node->Find("items")->GetString("type"), "string");
        EXPECT_TRUE(fallback->AsArray().empty());
        break;
    }
    if (field.kind == SettingKind::Integer || field.kind == SettingKind::Number) {
      EXPECT_EQ(field.minimum, node->GetNumber("minimum"));
      // The schema leaves some upper bounds open; the UI then stops at the int32 range.
      EXPECT_EQ(field.maximum, node->GetNumber("maximum").value_or(2147483647.0));
    }
  }
}

TEST(SettingsFieldsTest, EverySchemaKeyIsEditedOrListedAsUnedited) {
  std::set<std::string> leaves;
  CollectLeaves(Schema(), "", &leaves);
  std::multiset<std::string> covered;
  for (const auto& field : azookey::settings::GenericSettingFields()) {
    covered.insert(std::string(field.path));
  }
  for (const auto path : azookey::settings::DedicatedSettingPaths()) {
    covered.insert(std::string(path));
  }
  for (const auto& setting : azookey::settings::UneditedSettings()) {
    EXPECT_FALSE(setting.reason.empty());
    covered.insert(std::string(setting.path));
  }
  for (const auto& leaf : leaves) {
    EXPECT_EQ(covered.count(leaf), 1u) << leaf;
  }
  for (const auto& path : covered) {
    EXPECT_TRUE(leaves.contains(path)) << path << " is not a schema key";
  }
}

TEST(SettingsFieldsTest, EveryGeneratedControlHasItsStrings) {
  const auto names = ResourceNames();
  ASSERT_FALSE(names.empty());
  for (const auto& field : azookey::settings::GenericSettingFields()) {
    EXPECT_TRUE(names.contains(azookey::settings::SettingLabelResource(field.path))) << field.path;
    EXPECT_TRUE(names.contains(azookey::settings::SettingSectionResource(field.section)))
        << field.section;
    if (field.described) {
      EXPECT_TRUE(names.contains(azookey::settings::SettingDescriptionResource(field.path)))
          << field.path;
    }
    for (const auto option : field.options) {
      EXPECT_TRUE(names.contains(azookey::settings::SettingOptionResource(field.path, option)))
          << field.path << " " << option;
    }
  }
}

TEST(SettingsFieldsTest, ConditionsReferToSwitchesAndTheirValues) {
  for (const auto& field : azookey::settings::GenericSettingFields()) {
    if (!field.active_when) continue;
    SCOPED_TRACE(std::string(field.path));
    const auto* condition = azookey::settings::FindSettingField(field.active_when->path);
    ASSERT_NE(condition, nullptr);
    if (condition->kind == SettingKind::Bool) {
      EXPECT_TRUE(field.active_when->equals == "true" || field.active_when->equals == "false");
    } else {
      ASSERT_EQ(condition->kind, SettingKind::Enum);
      EXPECT_NE(std::find(condition->options.begin(), condition->options.end(),
                          field.active_when->equals),
                condition->options.end());
    }
  }
}

TEST(SettingsFieldsTest, PrivacyAxesAreActiveOnlyInCustomMode) {
  const auto* axis = azookey::settings::FindSettingField("privacy.custom.learning");
  ASSERT_NE(axis, nullptr);
  azookey::settings::SettingValues values;
  EXPECT_FALSE(azookey::settings::IsSettingFieldActive(*axis, values));
  values["privacy.mode"] = std::string("custom");
  EXPECT_TRUE(azookey::settings::IsSettingFieldActive(*axis, values));
  // An invalid stored mode counts as the default.
  values["privacy.mode"] = true;
  EXPECT_FALSE(azookey::settings::IsSettingFieldActive(*axis, values));
}

TEST(SettingsFieldsTest, ValuesAreCheckedAgainstTypeRangeAndOptions) {
  using azookey::settings::FindSettingField;
  using azookey::settings::ValidateSettingValue;
  const auto& candidates = *FindSettingField("maxCandidates");
  EXPECT_FALSE(ValidateSettingValue(candidates, int64_t{32}));
  EXPECT_TRUE(ValidateSettingValue(candidates, int64_t{0}));
  EXPECT_TRUE(ValidateSettingValue(candidates, 9.0));
  const auto& confidence = *FindSettingField("segmentBoundaryConfidence");
  EXPECT_FALSE(ValidateSettingValue(confidence, 1.0));
  EXPECT_TRUE(ValidateSettingValue(confidence, 1.5));
  EXPECT_TRUE(ValidateSettingValue(confidence, std::numeric_limits<double>::quiet_NaN()));
  const auto& mode = *FindSettingField("typoCorrectionMode");
  EXPECT_FALSE(ValidateSettingValue(mode, std::string("auto_replace")));
  EXPECT_TRUE(ValidateSettingValue(mode, std::string("aggressive")));
  EXPECT_TRUE(ValidateSettingValue(*FindSettingField("privacy.secureApps"), std::string("a")));
}

TEST(SettingsFieldsTest, OnlyValuesChangedFromWhatWasLoadedAreSaved) {
  azookey::settings::SettingValues loaded{
      {"maxCandidates", int64_t{12}},
      {"privacy.secureApps", std::vector<std::string>{"", " KeePass.exe"}},
  };
  azookey::settings::SettingValues edited{
      {"maxCandidates", int64_t{12}},
      {"privacy.secureApps", std::vector<std::string>{"KeePass.exe"}},
      // Shown at the default because the stored value was invalid for the field.
      {"autoUpdate.checkIntervalHours", int64_t{24}},
      {"inputMode", std::string("alnum_half")},
      {"liveConversion", false},
  };
  const auto changed = azookey::settings::ChangedSettingValues(edited, loaded);
  EXPECT_EQ(changed.size(), 1u);
  EXPECT_EQ(std::get<std::string>(changed.at("inputMode")), "alnum_half");
  // The text box shows the list trimmed; leaving it as shown is not an edit.
  EXPECT_FALSE(changed.contains("privacy.secureApps"));
  EXPECT_FALSE(changed.contains("maxCandidates"));
  EXPECT_FALSE(changed.contains("autoUpdate.checkIntervalHours"));
}

TEST(SettingsFieldsTest, ListTextIsOneTrimmedEntryPerLine) {
  using azookey::settings::FormatSettingList;
  using azookey::settings::ParseSettingList;
  EXPECT_EQ(ParseSettingList(" Code.exe \r\n\r\n\tdevenv.exe\rnotepad.exe\n"),
            (std::vector<std::string>{"Code.exe", "devenv.exe", "notepad.exe"}));
  EXPECT_TRUE(ParseSettingList("").empty());
  EXPECT_TRUE(ParseSettingList(" \r \n").empty());
  EXPECT_EQ(FormatSettingList({"a.exe", "b.exe"}), "a.exe\rb.exe");
  EXPECT_EQ(ParseSettingList(FormatSettingList({"a.exe", "b.exe"})),
            (std::vector<std::string>{"a.exe", "b.exe"}));
}
