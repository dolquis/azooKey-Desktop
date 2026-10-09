#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "AppProfiles.h"
#include "pch.h"

namespace azookey::settings {

// The "アプリ別" pane (app-profile-spec section 8): lists profilesByApp and adds, edits and deletes
// its entries, with candidateTagBoosts as a list of tag and multiplier rows. Changes stay in
// memory until the common save button; promptPrefixByApp is shown read-only below the list.
class ProfilesPane : public std::enable_shared_from_this<ProfilesPane> {
 public:
  struct Host {
    std::function<winrt::Microsoft::UI::Xaml::XamlRoot()> xaml_root;
  };

  void Build(winrt::Microsoft::UI::Xaml::Controls::StackPanel panel, Host host);
  // The profiles and legacy prefixes read from settings.json.
  void SetProfiles(AppProfiles profiles, std::map<std::string, std::string> legacy_prefixes);
  const AppProfiles& Profiles() const { return profiles_; }

 private:
  struct Editor;
  void Render();
  winrt::fire_and_forget Edit(std::optional<std::string> key);
  winrt::Microsoft::UI::Xaml::UIElement SummaryRow(const std::string& key,
                                                   const AppProfile& profile);

  Host host_;
  winrt::Microsoft::UI::Xaml::Controls::StackPanel list_panel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel legacy_panel_{nullptr};
  AppProfiles profiles_;
  std::map<std::string, std::string> legacy_prefixes_;
  bool editing_{false};
};

}  // namespace azookey::settings
