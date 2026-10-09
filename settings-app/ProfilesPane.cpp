// C++/WinRT requires the precompiled header first.
// clang-format off
#include "pch.h"
#include "ProfilesPane.h"
// clang-format on

#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace azookey::settings {
namespace {

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace controls = winrt::Microsoft::UI::Xaml::Controls;
using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

constexpr int kInherit = 0;
constexpr size_t kMaxLegacyPreviewChars = 60;

controls::TextBlock Block(const winrt::hstring& text, bool secondary = false) {
  controls::TextBlock block;
  block.Text(text);
  block.TextWrapping(xaml::TextWrapping::Wrap);
  if (secondary) {
    block.FontSize(12);
    block.Opacity(0.8);
  }
  return block;
}

void SetId(xaml::DependencyObject const& object, const std::wstring& id) {
  xaml::Automation::AutomationProperties::SetAutomationId(object, id);
}

std::wstring Wide(const std::string& utf8) { return std::wstring(winrt::to_hstring(utf8)); }

double RoundBoost(double value) { return std::round(value * 100.0) / 100.0; }

std::string BoostText(double value) {
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "%g", value);
  return buffer;
}

// A bool setting that can also be left unset: 0 inherits, 1 is on, 2 is off.
controls::ComboBox TriStateCombo(const ResourceLoader& resources, const winrt::hstring& header,
                                 const std::optional<bool>& value, const wchar_t* id) {
  controls::ComboBox combo;
  combo.Header(winrt::box_value(header));
  combo.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
  combo.Items().Append(winrt::box_value(resources.GetString(L"Profiles_Inherit")));
  combo.Items().Append(winrt::box_value(resources.GetString(L"Profiles_On")));
  combo.Items().Append(winrt::box_value(resources.GetString(L"Profiles_Off")));
  combo.SelectedIndex(!value ? kInherit : (*value ? 1 : 2));
  SetId(combo, id);
  return combo;
}

std::optional<bool> TriStateValue(const controls::ComboBox& combo) {
  const int index = combo.SelectedIndex();
  if (index == 1) return true;
  if (index == 2) return false;
  return std::nullopt;
}

// An enum setting that can also be left unset: 0 inherits, then the allowed values in order.
template <typename Options>
controls::ComboBox EnumCombo(const ResourceLoader& resources, const winrt::hstring& header,
                             const Options& options, const std::optional<std::string>& value,
                             const wchar_t* id) {
  controls::ComboBox combo;
  combo.Header(winrt::box_value(header));
  combo.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
  combo.Items().Append(winrt::box_value(resources.GetString(L"Profiles_Inherit")));
  int selected = kInherit;
  int index = 1;
  for (const auto option : options) {
    combo.Items().Append(winrt::box_value(winrt::to_hstring(std::string(option))));
    if (value && *value == option) selected = index;
    ++index;
  }
  combo.SelectedIndex(selected);
  SetId(combo, id);
  return combo;
}

template <typename Options>
std::optional<std::string> EnumValue(const controls::ComboBox& combo, const Options& options) {
  const int index = combo.SelectedIndex();
  if (index <= kInherit || static_cast<size_t>(index) > options.size()) return std::nullopt;
  return std::string(options[static_cast<size_t>(index) - 1]);
}

std::string Preview(const std::string& text) {
  // Cut on a character boundary so the preview stays valid UTF-8.
  size_t chars = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
      if (chars == kMaxLegacyPreviewChars) return text.substr(0, i) + "...";
      ++chars;
    }
  }
  return text;
}

}  // namespace

// The controls of the edit dialog and how they read back into a profile.
struct ProfilesPane::Editor {
  struct BoostRow {
    controls::StackPanel row{nullptr};
    controls::TextBox tag{nullptr};
    controls::NumberBox value{nullptr};
  };

  bool is_new{true};
  controls::TextBox key{nullptr};
  controls::TextBox name{nullptr};
  controls::ComboBox prediction{nullptr};
  controls::ComboBox sentence{nullptr};
  controls::ComboBox learning{nullptr};
  controls::ComboBox ai_backend{nullptr};
  controls::ToggleSwitch prefix_set{nullptr};
  controls::TextBox prefix{nullptr};
  controls::ComboBox style{nullptr};
  controls::ComboBox technical{nullptr};
  controls::ComboBox privacy{nullptr};
  controls::ComboBox bracket{nullptr};
  controls::StackPanel boosts_panel{nullptr};
  controls::TextBlock error_text{nullptr};
  std::vector<BoostRow> boost_rows;

  void AddBoostRow(const std::string& tag, double value) {
    ResourceLoader resources;
    BoostRow boost;
    boost.row = controls::StackPanel();
    boost.row.Orientation(controls::Orientation::Horizontal);
    boost.row.Spacing(8);
    boost.tag = controls::TextBox();
    boost.tag.Width(180);
    boost.tag.PlaceholderText(resources.GetString(L"Profiles_Boost_Tag"));
    boost.tag.Text(winrt::to_hstring(tag));
    SetId(boost.tag, L"ProfileBoostTag");
    boost.value = controls::NumberBox();
    boost.value.Width(120);
    boost.value.Minimum(kMinTagBoost);
    boost.value.Maximum(kMaxTagBoost);
    boost.value.SmallChange(0.1);
    boost.value.SpinButtonPlacementMode(controls::NumberBoxSpinButtonPlacementMode::Compact);
    // A typed value outside 1.0-3.0 is refused when the dialog is accepted, not clamped here.
    boost.value.ValidationMode(controls::NumberBoxValidationMode::Disabled);
    boost.value.Value(value);
    SetId(boost.value, L"ProfileBoostValue");
    controls::Button remove;
    remove.Content(winrt::box_value(resources.GetString(L"Profiles_Boost_Remove")));
    boost.row.Children().Append(boost.tag);
    boost.row.Children().Append(boost.value);
    boost.row.Children().Append(remove);
    boosts_panel.Children().Append(boost.row);
    const auto row = boost.row;
    remove.Click([this, row](auto const&, auto const&) {
      for (auto it = boost_rows.begin(); it != boost_rows.end(); ++it) {
        if (it->row == row) {
          boost_rows.erase(it);
          break;
        }
      }
      uint32_t at = 0;
      if (boosts_panel.Children().IndexOf(row, at)) boosts_panel.Children().RemoveAt(at);
    });
    boost_rows.push_back(std::move(boost));
  }

  // Reads the dialog into `*key` and `*profile`. On failure returns the string name of the
  // reason and leaves the dialog open.
  const wchar_t* Read(const AppProfiles& others, std::string* out_key, AppProfile* profile) const {
    if (is_new) {
      std::string typed = winrt::to_string(key.Text());
      const auto first = typed.find_first_not_of(" \t");
      if (first == std::string::npos) return L"Profiles_Error_KeyEmpty";
      typed = typed.substr(first, typed.find_last_not_of(" \t") - first + 1);
      for (const auto& [existing, ignored] : others) {
        if (SameProfileKey(existing, typed)) return L"Profiles_Error_KeyDuplicate";
      }
      *out_key = std::move(typed);
    }
    AppProfile read;
    if (const auto text = winrt::to_string(name.Text()); !text.empty()) read.profile_name = text;
    read.prediction_enabled = TriStateValue(prediction);
    read.sentence_completion = TriStateValue(sentence);
    read.learning_enabled = TriStateValue(learning);
    read.ai_backend = EnumValue(ai_backend, kProfileAiBackends);
    if (prefix_set.IsOn()) read.prompt_prefix = winrt::to_string(prefix.Text());
    read.style = EnumValue(style, kProfileStyles);
    read.prefer_technical_terms = TriStateValue(technical);
    read.privacy_mode = EnumValue(privacy, kProfilePrivacyModes);
    read.bracket_pairing = EnumValue(bracket, kProfileBracketPairings);
    for (const auto& boost : boost_rows) {
      std::string tag = winrt::to_string(boost.tag.Text());
      const auto first = tag.find_first_not_of(" \t");
      if (first == std::string::npos) return L"Profiles_Error_TagEmpty";
      tag = tag.substr(first, tag.find_last_not_of(" \t") - first + 1);
      const double value = RoundBoost(boost.value.Value());
      if (!std::isfinite(value) || value < kMinTagBoost || value > kMaxTagBoost) {
        return L"Profiles_Error_BoostRange";
      }
      if (!read.candidate_tag_boosts.emplace(tag, value).second) {
        return L"Profiles_Error_TagDuplicate";
      }
    }
    *profile = std::move(read);
    return nullptr;
  }
};

void ProfilesPane::Build(controls::StackPanel panel, Host host) {
  host_ = std::move(host);
  ResourceLoader resources;

  controls::TextBlock title;
  title.Text(resources.GetString(L"Profiles_Title"));
  title.FontSize(20);
  title.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
  panel.Children().Append(title);
  panel.Children().Append(Block(resources.GetString(L"Profiles_Description"), true));
  panel.Children().Append(Block(resources.GetString(L"Profiles_UnsavedNote"), true));

  controls::Button add;
  add.Content(winrt::box_value(resources.GetString(L"Profiles_AddButton")));
  SetId(add, L"ProfilesAddButton");
  add.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (const auto self = weak.lock()) self->Edit(std::nullopt);
  });
  panel.Children().Append(add);

  list_panel_ = controls::StackPanel();
  list_panel_.Spacing(8);
  SetId(list_panel_, L"ProfilesList");
  panel.Children().Append(list_panel_);

  controls::TextBlock legacy_heading;
  legacy_heading.Text(resources.GetString(L"Profiles_LegacyHeading"));
  legacy_heading.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
  legacy_heading.Margin(xaml::ThicknessHelper::FromLengths(0, 12, 0, 0));
  panel.Children().Append(legacy_heading);
  panel.Children().Append(Block(resources.GetString(L"Profiles_LegacyNote"), true));
  legacy_panel_ = controls::StackPanel();
  legacy_panel_.Spacing(4);
  SetId(legacy_panel_, L"ProfilesLegacyList");
  panel.Children().Append(legacy_panel_);
  Render();
}

void ProfilesPane::SetProfiles(AppProfiles profiles,
                               std::map<std::string, std::string> legacy_prefixes) {
  profiles_ = std::move(profiles);
  legacy_prefixes_ = std::move(legacy_prefixes);
  if (list_panel_) Render();
}

xaml::UIElement ProfilesPane::SummaryRow(const std::string& key, const AppProfile& profile) {
  ResourceLoader resources;
  const auto on_off = [&resources](bool value) {
    return std::wstring(resources.GetString(value ? L"Profiles_On" : L"Profiles_Off"));
  };
  std::wstring summary;
  const auto add = [&summary](const std::wstring& part) {
    if (!summary.empty()) summary += L"  ";
    summary += part;
  };
  const auto label = [&resources](const wchar_t* name) {
    return std::wstring(resources.GetString(name)) + L": ";
  };
  if (profile.profile_name && !profile.profile_name->empty()) {
    add(label(L"Profiles_Summary_Name") + Wide(*profile.profile_name));
  }
  if (profile.prediction_enabled) {
    add(label(L"Profiles_Summary_Prediction") + on_off(*profile.prediction_enabled));
  }
  if (profile.sentence_completion) {
    add(label(L"Profiles_Summary_Sentence") + on_off(*profile.sentence_completion));
  }
  if (profile.learning_enabled) {
    add(label(L"Profiles_Summary_Learning") + on_off(*profile.learning_enabled));
  }
  if (profile.ai_backend) add(label(L"Profiles_Summary_Ai") + Wide(*profile.ai_backend));
  if (profile.prompt_prefix) {
    add(label(L"Profiles_Summary_Prefix") +
        (profile.prompt_prefix->empty()
             ? std::wstring(resources.GetString(L"Profiles_PrefixCleared"))
             : Wide(Preview(*profile.prompt_prefix))));
  }
  if (profile.style) add(label(L"Profiles_Summary_Style") + Wide(*profile.style));
  if (profile.prefer_technical_terms) {
    add(label(L"Profiles_Summary_Technical") + on_off(*profile.prefer_technical_terms));
  }
  if (profile.privacy_mode) add(label(L"Profiles_Summary_Privacy") + Wide(*profile.privacy_mode));
  if (profile.bracket_pairing) {
    add(label(L"Profiles_Summary_Bracket") + Wide(*profile.bracket_pairing));
  }
  if (!profile.candidate_tag_boosts.empty()) {
    std::wstring boosts;
    for (const auto& [tag, boost] : profile.candidate_tag_boosts) {
      if (!boosts.empty()) boosts += L", ";
      boosts += Wide(tag) + L" x" + Wide(BoostText(boost));
    }
    add(label(L"Profiles_Summary_Boosts") + boosts);
  }
  if (summary.empty()) summary = resources.GetString(L"Profiles_InheritsAll");

  controls::Grid grid;
  grid.ColumnSpacing(12);
  controls::ColumnDefinition text_column;
  text_column.Width(xaml::GridLengthHelper::FromValueAndType(1, xaml::GridUnitType::Star));
  grid.ColumnDefinitions().Append(text_column);
  controls::ColumnDefinition button_column;
  button_column.Width(xaml::GridLengthHelper::Auto());
  grid.ColumnDefinitions().Append(button_column);

  controls::StackPanel text;
  text.Spacing(2);
  controls::TextBlock key_block;
  key_block.Text(winrt::to_hstring(key));
  key_block.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
  key_block.IsTextSelectionEnabled(true);
  text.Children().Append(key_block);
  text.Children().Append(Block(winrt::hstring(summary), true));
  grid.Children().Append(text);

  controls::StackPanel buttons;
  buttons.Orientation(controls::Orientation::Horizontal);
  buttons.Spacing(8);
  controls::Button edit;
  edit.Content(winrt::box_value(resources.GetString(L"Profiles_EditButton")));
  SetId(edit, L"ProfileEditButton_" + Wide(key));
  edit.Click([weak = weak_from_this(), key](auto const&, auto const&) {
    if (const auto self = weak.lock()) self->Edit(key);
  });
  buttons.Children().Append(edit);
  controls::Button remove;
  remove.Content(winrt::box_value(resources.GetString(L"Profiles_DeleteButton")));
  SetId(remove, L"ProfileDeleteButton_" + Wide(key));
  remove.Click([weak = weak_from_this(), key](auto const&, auto const&) {
    if (const auto self = weak.lock()) {
      self->profiles_.erase(key);
      self->Render();
    }
  });
  buttons.Children().Append(remove);
  controls::Grid::SetColumn(buttons, 1);
  grid.Children().Append(buttons);
  return grid;
}

void ProfilesPane::Render() {
  ResourceLoader resources;
  list_panel_.Children().Clear();
  if (profiles_.empty()) {
    list_panel_.Children().Append(Block(resources.GetString(L"Profiles_Empty"), true));
  }
  for (const auto& [key, profile] : profiles_) {
    list_panel_.Children().Append(SummaryRow(key, profile));
  }
  legacy_panel_.Children().Clear();
  if (legacy_prefixes_.empty()) {
    legacy_panel_.Children().Append(Block(resources.GetString(L"Profiles_LegacyEmpty"), true));
  }
  for (const auto& [app, prefix] : legacy_prefixes_) {
    // Moving copies the value into the matching profile's promptPrefix; the legacy key stays
    // (app-profile-spec section 6). A profile that already sets promptPrefix wins, so the button
    // is off for it.
    bool migrated = false;
    for (const auto& [key, profile] : profiles_) {
      if (SameProfileKey(key, app) && profile.prompt_prefix) migrated = true;
    }
    controls::Grid row;
    row.ColumnSpacing(12);
    controls::ColumnDefinition text_column;
    text_column.Width(xaml::GridLengthHelper::FromValueAndType(1, xaml::GridUnitType::Star));
    row.ColumnDefinitions().Append(text_column);
    controls::ColumnDefinition button_column;
    button_column.Width(xaml::GridLengthHelper::Auto());
    row.ColumnDefinitions().Append(button_column);
    auto text = Block(winrt::to_hstring(app + ": " + Preview(prefix)), true);
    text.IsTextSelectionEnabled(true);
    row.Children().Append(text);
    controls::Button move;
    move.Content(winrt::box_value(
        resources.GetString(migrated ? L"Profiles_Migrated" : L"Profiles_MigrateButton")));
    move.IsEnabled(!migrated);
    SetId(move, L"ProfileMigrateButton_" + Wide(app));
    move.Click([weak = weak_from_this(), app, prefix](auto const&, auto const&) {
      const auto self = weak.lock();
      if (!self) return;
      auto target = self->profiles_.end();
      for (auto it = self->profiles_.begin(); it != self->profiles_.end(); ++it) {
        if (SameProfileKey(it->first, app)) target = it;
      }
      if (target == self->profiles_.end())
        target = self->profiles_.emplace(app, AppProfile{}).first;
      if (!target->second.prompt_prefix) target->second.prompt_prefix = prefix;
      self->Render();
    });
    controls::Grid::SetColumn(move, 1);
    row.Children().Append(move);
    legacy_panel_.Children().Append(row);
  }
}

winrt::fire_and_forget ProfilesPane::Edit(std::optional<std::string> key) {
  const auto self = shared_from_this();
  if (editing_) co_return;
  editing_ = true;
  try {
    ResourceLoader resources;
    const AppProfile current = key ? profiles_.at(*key) : AppProfile{};
    auto editor = std::make_shared<Editor>();
    editor->is_new = !key.has_value();

    controls::StackPanel form;
    form.Spacing(12);
    editor->key = controls::TextBox();
    editor->key.Header(winrt::box_value(resources.GetString(L"Profiles_Key_Header")));
    editor->key.PlaceholderText(resources.GetString(L"Profiles_Key_Placeholder"));
    editor->key.Text(winrt::to_hstring(key.value_or("")));
    editor->key.IsEnabled(editor->is_new);
    SetId(editor->key, L"ProfileKeyBox");
    form.Children().Append(editor->key);
    editor->name = controls::TextBox();
    editor->name.Header(winrt::box_value(resources.GetString(L"Profiles_Name_Header")));
    editor->name.Text(winrt::to_hstring(current.profile_name.value_or("")));
    SetId(editor->name, L"ProfileNameBox");
    form.Children().Append(editor->name);

    editor->prediction =
        TriStateCombo(resources, resources.GetString(L"Profiles_Prediction_Header"),
                      current.prediction_enabled, L"ProfilePredictionCombo");
    form.Children().Append(editor->prediction);
    editor->sentence = TriStateCombo(resources, resources.GetString(L"Profiles_Sentence_Header"),
                                     current.sentence_completion, L"ProfileSentenceCombo");
    form.Children().Append(editor->sentence);
    editor->learning = TriStateCombo(resources, resources.GetString(L"Profiles_Learning_Header"),
                                     current.learning_enabled, L"ProfileLearningCombo");
    form.Children().Append(editor->learning);
    editor->ai_backend =
        EnumCombo(resources, resources.GetString(L"Profiles_AiBackend_Header"), kProfileAiBackends,
                  current.ai_backend, L"ProfileAiBackendCombo");
    form.Children().Append(editor->ai_backend);

    editor->prefix_set = controls::ToggleSwitch();
    editor->prefix_set.Header(
        winrt::box_value(resources.GetString(L"Profiles_PromptPrefix_Toggle")));
    editor->prefix_set.IsOn(current.prompt_prefix.has_value());
    SetId(editor->prefix_set, L"ProfilePromptPrefixToggle");
    form.Children().Append(editor->prefix_set);
    editor->prefix = controls::TextBox();
    editor->prefix.Header(winrt::box_value(resources.GetString(L"Profiles_PromptPrefix_Header")));
    editor->prefix.AcceptsReturn(true);
    editor->prefix.TextWrapping(xaml::TextWrapping::Wrap);
    editor->prefix.MinHeight(72);
    editor->prefix.Text(winrt::to_hstring(current.prompt_prefix.value_or("")));
    editor->prefix.IsEnabled(editor->prefix_set.IsOn());
    SetId(editor->prefix, L"ProfilePromptPrefixBox");
    form.Children().Append(editor->prefix);
    form.Children().Append(Block(resources.GetString(L"Profiles_PromptPrefix_Note"), true));
    editor->prefix_set.Toggled([editor](auto const&, auto const&) {
      editor->prefix.IsEnabled(editor->prefix_set.IsOn());
    });

    editor->style = EnumCombo(resources, resources.GetString(L"Profiles_Style_Header"),
                              kProfileStyles, current.style, L"ProfileStyleCombo");
    form.Children().Append(editor->style);
    editor->technical = TriStateCombo(resources, resources.GetString(L"Profiles_Technical_Header"),
                                      current.prefer_technical_terms, L"ProfileTechnicalCombo");
    form.Children().Append(editor->technical);
    editor->privacy = EnumCombo(resources, resources.GetString(L"Profiles_Privacy_Header"),
                                kProfilePrivacyModes, current.privacy_mode, L"ProfilePrivacyCombo");
    form.Children().Append(editor->privacy);
    editor->bracket =
        EnumCombo(resources, resources.GetString(L"Profiles_Bracket_Header"),
                  kProfileBracketPairings, current.bracket_pairing, L"ProfileBracketCombo");
    form.Children().Append(editor->bracket);

    controls::TextBlock boosts_header;
    boosts_header.Text(resources.GetString(L"Profiles_Boosts_Header"));
    boosts_header.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
    form.Children().Append(boosts_header);
    form.Children().Append(Block(resources.GetString(L"Profiles_Boosts_Note"), true));
    editor->boosts_panel = controls::StackPanel();
    editor->boosts_panel.Spacing(6);
    form.Children().Append(editor->boosts_panel);
    for (const auto& [tag, boost] : current.candidate_tag_boosts) editor->AddBoostRow(tag, boost);
    controls::Button add_boost;
    add_boost.Content(winrt::box_value(resources.GetString(L"Profiles_Boost_Add")));
    SetId(add_boost, L"ProfileBoostAddButton");
    add_boost.Click([editor](auto const&, auto const&) { editor->AddBoostRow("", kMinTagBoost); });
    form.Children().Append(add_boost);

    editor->error_text = Block(L"");
    editor->error_text.Foreground(
        xaml::Media::SolidColorBrush(winrt::Windows::UI::Colors::OrangeRed()));
    SetId(editor->error_text, L"ProfileEditorError");
    form.Children().Append(editor->error_text);

    controls::ScrollViewer scroller;
    scroller.MaxHeight(520);
    scroller.Content(form);

    controls::ContentDialog dialog;
    dialog.XamlRoot(host_.xaml_root());
    dialog.Title(winrt::box_value(
        resources.GetString(key ? L"Profiles_EditorTitleEdit" : L"Profiles_EditorTitleNew")));
    dialog.Content(scroller);
    dialog.PrimaryButtonText(resources.GetString(L"Profiles_Apply"));
    dialog.CloseButtonText(resources.GetString(L"Profiles_Cancel"));
    dialog.DefaultButton(controls::ContentDialogButton::Primary);

    std::string result_key = key.value_or("");
    AppProfile result_profile;
    dialog.PrimaryButtonClick([this, editor, &result_key, &result_profile](
                                  controls::ContentDialog const&,
                                  controls::ContentDialogButtonClickEventArgs const& args) {
      ResourceLoader strings;
      std::string read_key = result_key;
      AppProfile read_profile;
      if (const auto* problem = editor->Read(profiles_, &read_key, &read_profile)) {
        editor->error_text.Text(strings.GetString(problem));
        args.Cancel(true);
        return;
      }
      result_key = std::move(read_key);
      result_profile = std::move(read_profile);
    });

    if (co_await dialog.ShowAsync() == controls::ContentDialogResult::Primary) {
      profiles_[result_key] = std::move(result_profile);
      Render();
    }
  } catch (...) {
  }
  editing_ = false;
}

}  // namespace azookey::settings
