#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "azookey/ipc/Payloads.h"
#include "pch.h"

namespace azookey::settings {

// The "学習" pane (learning-data-management-spec section 3): the entries of each store with
// search, per-entry forgetting, and export / import of a backup archive. Every operation is an
// IPC request to the Host, which is the only writer of the data files. Forgetting and importing
// ask for confirmation first.
class LearningPane : public std::enable_shared_from_this<LearningPane> {
 public:
  struct Host {
    std::function<winrt::Microsoft::UI::Xaml::XamlRoot()> xaml_root;
    std::function<HWND()> window_handle;
  };

  void Build(winrt::Microsoft::UI::Xaml::Controls::StackPanel panel,
             winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher, Host host);
  // Called each time the pane is shown; the first call lists the first store.
  void OnShown();

 private:
  void SelectTab(int index);
  winrt::fire_and_forget LoadPage();
  winrt::fire_and_forget Forget(azookey::ipc::LearningEntryField entry);
  winrt::fire_and_forget Export();
  winrt::fire_and_forget Import();
  winrt::Windows::Foundation::IAsyncOperation<bool> Confirm(winrt::hstring title,
                                                            winrt::hstring message,
                                                            winrt::hstring primary_text);
  void RenderEntries();
  void UpdatePager();
  void SetBusy(bool busy);
  void RecoverFromFailure();
  void ShowStatus(winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
                  const winrt::hstring& title, const winrt::hstring& message);
  std::vector<std::string> CheckedStores() const;

  winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{nullptr};
  Host host_;
  winrt::Microsoft::UI::Xaml::Controls::RadioButtons tabs_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::InfoBar status_bar_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel entries_section_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel backup_section_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox search_box_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button search_button_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel entries_panel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock page_text_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button previous_button_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button next_button_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch encrypt_toggle_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::InfoBar plaintext_bar_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ComboBox conflict_combo_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button export_button_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button import_button_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock backup_result_{nullptr};
  std::vector<winrt::Microsoft::UI::Xaml::Controls::CheckBox> store_checks_;

  int tab_index_{0};
  std::string query_;
  uint64_t offset_{0};
  uint64_t total_{0};
  // Bumped whenever the listing changes, so a late response for an older one is dropped.
  uint64_t generation_{0};
  std::vector<azookey::ipc::LearningEntryField> entries_;
  bool loaded_once_{false};
  bool busy_{false};
};

}  // namespace azookey::settings
