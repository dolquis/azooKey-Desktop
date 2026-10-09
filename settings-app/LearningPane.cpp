// C++/WinRT requires the precompiled header first.
// clang-format off
#include "pch.h"
#include "LearningPane.h"
// clang-format on

#include <shobjidl_core.h>
#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>

#include <filesystem>
#include <string>

#include "LearningPaneModel.h"
#include "ModelPaneModel.h"
#include "SettingsIpcClient.h"
#include "UiDispatch.h"

namespace azookey::settings {
namespace {

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace controls = winrt::Microsoft::UI::Xaml::Controls;
using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

winrt::hstring Str(const ResourceLoader& resources, const std::string& name) {
  return resources.GetString(winrt::to_hstring(name));
}

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

// One table row: the cells in the shared column widths, then an optional trailing control.
controls::Grid TableRow(const std::vector<winrt::hstring>& cells, bool header,
                        xaml::UIElement const& action) {
  controls::Grid grid;
  grid.ColumnSpacing(12);
  static constexpr double kStar[] = {1.2, 1.5, 0.0, 0.0, 1.0};
  static constexpr double kFixed[] = {0.0, 0.0, 56.0, 96.0, 0.0};
  for (size_t i = 0; i < 5; ++i) {
    controls::ColumnDefinition column;
    column.Width(kFixed[i] > 0 ? xaml::GridLengthHelper::FromPixels(kFixed[i])
                               : xaml::GridLengthHelper::FromValueAndType(
                                     kStar[i], xaml::GridUnitType::Star));
    grid.ColumnDefinitions().Append(column);
  }
  controls::ColumnDefinition action_column;
  action_column.Width(xaml::GridLengthHelper::FromPixels(72));
  grid.ColumnDefinitions().Append(action_column);
  for (size_t i = 0; i < cells.size() && i < 5; ++i) {
    auto cell = Block(cells[i], !header);
    if (header) cell.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
    controls::Grid::SetColumn(cell, static_cast<int32_t>(i));
    grid.Children().Append(cell);
  }
  if (action) {
    controls::Grid::SetColumn(action.as<xaml::FrameworkElement>(), 5);
    grid.Children().Append(action);
  }
  return grid;
}

}  // namespace

void LearningPane::Build(controls::StackPanel panel,
                         winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher, Host host) {
  dispatcher_ = std::move(dispatcher);
  host_ = std::move(host);
  ResourceLoader resources;

  controls::TextBlock title;
  title.Text(resources.GetString(L"Learning_Title"));
  title.FontSize(20);
  title.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
  panel.Children().Append(title);
  panel.Children().Append(Block(resources.GetString(L"Learning_Description"), true));

  status_bar_ = controls::InfoBar();
  status_bar_.IsOpen(false);
  status_bar_.IsClosable(true);
  SetId(status_bar_, L"LearningStatusInfoBar");
  panel.Children().Append(status_bar_);

  tabs_ = controls::RadioButtons();
  tabs_.MaxColumns(5);
  SetId(tabs_, L"LearningTabs");
  for (const auto& tab : kLearningStoreTabs) {
    tabs_.Items().Append(winrt::box_value(Str(resources, std::string(tab.resource))));
  }
  tabs_.Items().Append(winrt::box_value(resources.GetString(L"LearningStore_backup")));
  tabs_.SelectedIndex(0);
  tabs_.SelectionChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (const auto self = weak.lock()) self->SelectTab(self->tabs_.SelectedIndex());
  });
  panel.Children().Append(tabs_);

  // Entries of one store.
  entries_section_ = controls::StackPanel();
  entries_section_.Spacing(8);
  controls::StackPanel search_row;
  search_row.Orientation(controls::Orientation::Horizontal);
  search_row.Spacing(8);
  search_box_ = controls::TextBox();
  search_box_.Width(280);
  search_box_.PlaceholderText(resources.GetString(L"Learning_SearchPlaceholder"));
  SetId(search_box_, L"LearningSearchBox");
  search_box_.KeyDown(
      [weak = weak_from_this()](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
        if (args.Key() != winrt::Windows::System::VirtualKey::Enter) return;
        if (const auto self = weak.lock()) {
          self->query_ = winrt::to_string(self->search_box_.Text());
          self->offset_ = 0;
          self->LoadPage();
        }
      });
  search_row.Children().Append(search_box_);
  search_button_ = controls::Button();
  search_button_.Content(winrt::box_value(resources.GetString(L"Learning_SearchButton")));
  SetId(search_button_, L"LearningSearchButton");
  search_button_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (const auto self = weak.lock()) {
      self->query_ = winrt::to_string(self->search_box_.Text());
      self->offset_ = 0;
      self->LoadPage();
    }
  });
  search_row.Children().Append(search_button_);
  entries_section_.Children().Append(search_row);

  entries_panel_ = controls::StackPanel();
  entries_panel_.Spacing(6);
  SetId(entries_panel_, L"LearningEntries");
  entries_section_.Children().Append(entries_panel_);

  controls::StackPanel pager;
  pager.Orientation(controls::Orientation::Horizontal);
  pager.Spacing(8);
  previous_button_ = controls::Button();
  previous_button_.Content(winrt::box_value(resources.GetString(L"Learning_PreviousPage")));
  SetId(previous_button_, L"LearningPreviousPageButton");
  previous_button_.Click([weak = weak_from_this()](auto const&, auto const&) {
    const auto self = weak.lock();
    if (!self || self->offset_ == 0) return;
    self->offset_ = self->offset_ > kLearningPageSize ? self->offset_ - kLearningPageSize : 0;
    self->LoadPage();
  });
  pager.Children().Append(previous_button_);
  next_button_ = controls::Button();
  next_button_.Content(winrt::box_value(resources.GetString(L"Learning_NextPage")));
  SetId(next_button_, L"LearningNextPageButton");
  next_button_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (const auto self = weak.lock()) {
      self->offset_ += kLearningPageSize;
      self->LoadPage();
    }
  });
  pager.Children().Append(next_button_);
  page_text_ = Block(L"", true);
  page_text_.VerticalAlignment(xaml::VerticalAlignment::Center);
  SetId(page_text_, L"LearningPageText");
  pager.Children().Append(page_text_);
  entries_section_.Children().Append(pager);
  panel.Children().Append(entries_section_);

  // Backup: export and import.
  backup_section_ = controls::StackPanel();
  backup_section_.Spacing(12);
  backup_section_.Visibility(xaml::Visibility::Collapsed);
  backup_section_.Children().Append(Block(resources.GetString(L"Learning_BackupStores"), true));
  for (const auto& tab : kLearningStoreTabs) {
    controls::CheckBox check;
    check.Content(winrt::box_value(Str(resources, std::string(tab.resource))));
    check.IsChecked(true);
    check.Tag(winrt::box_value(winrt::to_hstring(std::string(tab.id))));
    SetId(check, L"LearningBackupStore_" + std::wstring(winrt::to_hstring(std::string(tab.id))));
    store_checks_.push_back(check);
    backup_section_.Children().Append(check);
  }

  encrypt_toggle_ = controls::ToggleSwitch();
  encrypt_toggle_.Header(winrt::box_value(resources.GetString(L"Learning_EncryptToggle")));
  encrypt_toggle_.IsOn(true);
  SetId(encrypt_toggle_, L"LearningEncryptToggle");
  encrypt_toggle_.Toggled([weak = weak_from_this()](auto const&, auto const&) {
    if (const auto self = weak.lock()) self->plaintext_bar_.IsOpen(!self->encrypt_toggle_.IsOn());
  });
  backup_section_.Children().Append(encrypt_toggle_);
  plaintext_bar_ = controls::InfoBar();
  plaintext_bar_.Severity(controls::InfoBarSeverity::Warning);
  plaintext_bar_.IsClosable(false);
  plaintext_bar_.IsOpen(false);
  plaintext_bar_.Title(resources.GetString(L"Learning_PlaintextTitle"));
  plaintext_bar_.Message(resources.GetString(L"Learning_PlaintextMessage"));
  SetId(plaintext_bar_, L"LearningPlaintextInfoBar");
  backup_section_.Children().Append(plaintext_bar_);

  export_button_ = controls::Button();
  export_button_.Content(winrt::box_value(resources.GetString(L"Learning_ExportButton")));
  SetId(export_button_, L"LearningExportButton");
  export_button_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (const auto self = weak.lock()) self->Export();
  });
  backup_section_.Children().Append(export_button_);

  conflict_combo_ = controls::ComboBox();
  conflict_combo_.Header(winrt::box_value(resources.GetString(L"Learning_ConflictHeader")));
  for (const auto option : kConflictResolutions) {
    conflict_combo_.Items().Append(
        winrt::box_value(Str(resources, "LearningConflict_" + std::string(option))));
  }
  conflict_combo_.SelectedIndex(0);
  SetId(conflict_combo_, L"LearningConflictCombo");
  backup_section_.Children().Append(conflict_combo_);
  import_button_ = controls::Button();
  import_button_.Content(winrt::box_value(resources.GetString(L"Learning_ImportButton")));
  SetId(import_button_, L"LearningImportButton");
  import_button_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (const auto self = weak.lock()) self->Import();
  });
  backup_section_.Children().Append(import_button_);

  backup_result_ = Block(L"", true);
  SetId(backup_result_, L"LearningBackupResult");
  backup_section_.Children().Append(backup_result_);
  panel.Children().Append(backup_section_);

  UpdatePager();
}

void LearningPane::OnShown() {
  if (!loaded_once_ && tab_index_ < static_cast<int>(kLearningStoreTabs.size())) LoadPage();
}

void LearningPane::SelectTab(int index) {
  if (index < 0 || index == tab_index_) return;
  tab_index_ = index;
  const bool backup = index >= static_cast<int>(kLearningStoreTabs.size());
  entries_section_.Visibility(backup ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);
  backup_section_.Visibility(backup ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
  status_bar_.IsOpen(false);
  if (backup) {
    ++generation_;
    return;
  }
  query_.clear();
  search_box_.Text(L"");
  offset_ = 0;
  total_ = 0;
  entries_.clear();
  RenderEntries();
  LoadPage();
}

void LearningPane::SetBusy(bool busy) {
  busy_ = busy;
  search_button_.IsEnabled(!busy);
  export_button_.IsEnabled(!busy);
  import_button_.IsEnabled(!busy);
  if (busy) {
    previous_button_.IsEnabled(false);
    next_button_.IsEnabled(false);
  } else {
    UpdatePager();
  }
}

void LearningPane::ShowStatus(controls::InfoBarSeverity severity, const winrt::hstring& title,
                              const winrt::hstring& message) {
  status_bar_.Severity(severity);
  status_bar_.Title(title);
  status_bar_.Message(message);
  status_bar_.IsOpen(true);
}

std::vector<std::string> LearningPane::CheckedStores() const {
  std::vector<std::string> stores;
  for (const auto& check : store_checks_) {
    if (check.IsChecked().Value()) {
      stores.push_back(winrt::to_string(winrt::unbox_value<winrt::hstring>(check.Tag())));
    }
  }
  return stores;
}

void LearningPane::UpdatePager() {
  const auto range = ComputeLearningPage(offset_, entries_.size(), total_);
  previous_button_.IsEnabled(!busy_ && range.has_previous);
  next_button_.IsEnabled(!busy_ && range.has_next);
  ResourceLoader resources;
  if (entries_.empty()) {
    page_text_.Text(total_ == 0 ? resources.GetString(L"Learning_NoEntries") : winrt::hstring());
    return;
  }
  page_text_.Text(winrt::to_hstring(std::to_string(range.first) + "-" + std::to_string(range.last) +
                                    " / " + std::to_string(total_)));
}

winrt::fire_and_forget LearningPane::LoadPage() {
  const auto self = shared_from_this();
  if (tab_index_ >= static_cast<int>(kLearningStoreTabs.size())) co_return;
  const auto generation = ++generation_;
  azookey::ipc::ListLearningEntriesRequest request;
  request.store = std::string(kLearningStoreTabs[static_cast<size_t>(tab_index_)].id);
  request.query = query_;
  request.limit = kLearningPageSize;
  request.offset = offset_;
  loaded_once_ = true;
  SetBusy(true);
  status_bar_.IsOpen(false);
  const auto dispatcher = dispatcher_;
  try {
    co_await winrt::resume_background();
    const auto result = RequestListLearningEntries(DefaultSettingsIpcOptions(), request);
    co_await ResumeForeground(dispatcher);

    if (generation != generation_) co_return;
    SetBusy(false);
    ResourceLoader resources;
    if (result.status != HostCallStatus::Ok) {
      ShowStatus(controls::InfoBarSeverity::Error, resources.GetString(L"Learning_ProblemTitle"),
                 Str(resources, HostCallStatusResource(result.status)));
      co_return;
    }
    if (!result.response->ok) {
      ShowStatus(controls::InfoBarSeverity::Error, resources.GetString(L"Learning_ProblemTitle"),
                 Str(resources, LearningErrorResource(result.response->error.value_or(""))));
      co_return;
    }
    entries_ = result.response->entries;
    total_ = result.response->total;
    RenderEntries();
  } catch (...) {
    SetBusy(false);
  }
}

void LearningPane::RenderEntries() {
  ResourceLoader resources;
  entries_panel_.Children().Clear();
  if (!entries_.empty()) {
    entries_panel_.Children().Append(TableRow(
        {resources.GetString(L"Learning_Col_Reading"), resources.GetString(L"Learning_Col_Surface"),
         resources.GetString(L"Learning_Col_Weight"), resources.GetString(L"Learning_Col_LastUsed"),
         resources.GetString(L"Learning_Col_Tags")},
        true, nullptr));
  }
  for (const auto& entry : entries_) {
    controls::Button forget_button;
    forget_button.Content(winrt::box_value(resources.GetString(L"Learning_ForgetButton")));
    SetId(forget_button, L"LearningForgetButton_" + std::wstring(winrt::to_hstring(entry.id)));
    forget_button.Click([weak = weak_from_this(), entry](auto const&, auto const&) {
      if (const auto self = weak.lock()) self->Forget(entry);
    });
    entries_panel_.Children().Append(
        TableRow({winrt::to_hstring(entry.reading), winrt::to_hstring(entry.surface),
                  winrt::to_hstring(FormatLearningWeight(entry.weight)),
                  winrt::to_hstring(FormatLearningDate(entry.last_updated_epoch_sec)),
                  winrt::to_hstring(JoinLearningTags(entry.tags))},
                 false, forget_button));
  }
  UpdatePager();
}

winrt::Windows::Foundation::IAsyncOperation<bool> LearningPane::Confirm(
    winrt::hstring title, winrt::hstring message, winrt::hstring primary_text) {
  ResourceLoader resources;
  controls::ContentDialog dialog;
  dialog.XamlRoot(host_.xaml_root());
  dialog.Title(winrt::box_value(title));
  dialog.Content(winrt::box_value(message));
  dialog.PrimaryButtonText(primary_text);
  dialog.CloseButtonText(resources.GetString(L"Learning_CancelButton"));
  dialog.DefaultButton(controls::ContentDialogButton::Close);
  const auto result = co_await dialog.ShowAsync();
  co_return result == controls::ContentDialogResult::Primary;
}

winrt::fire_and_forget LearningPane::Forget(azookey::ipc::LearningEntryField entry) {
  const auto self = shared_from_this();
  if (busy_) co_return;
  const auto dispatcher = dispatcher_;
  try {
    ResourceLoader resources;
    const auto message = resources.GetString(L"Learning_ForgetConfirmMessage") + L"\n" +
                         winrt::to_hstring(entry.reading) + L" / " +
                         winrt::to_hstring(entry.surface);
    if (!co_await Confirm(resources.GetString(L"Learning_ForgetConfirmTitle"), message,
                          resources.GetString(L"Learning_ForgetButton"))) {
      co_return;
    }
    SetBusy(true);
    azookey::ipc::ForgetLearningEntryRequest request;
    request.store = std::string(kLearningStoreTabs[static_cast<size_t>(tab_index_)].id);
    request.id = entry.id;
    co_await winrt::resume_background();
    const auto result = RequestForgetLearningEntry(DefaultSettingsIpcOptions(), request);
    co_await ResumeForeground(dispatcher);

    SetBusy(false);
    ResourceLoader after;
    if (result.status != HostCallStatus::Ok) {
      ShowStatus(controls::InfoBarSeverity::Error, after.GetString(L"Learning_ProblemTitle"),
                 Str(after, HostCallStatusResource(result.status)));
      co_return;
    }
    if (!result.response->ok) {
      ShowStatus(controls::InfoBarSeverity::Error, after.GetString(L"Learning_ProblemTitle"),
                 Str(after, LearningErrorResource(result.response->error.value_or(""))));
      co_return;
    }
    ShowStatus(controls::InfoBarSeverity::Success, after.GetString(L"Learning_ForgotTitle"),
               after.GetString(result.response->removed ? L"Learning_ForgotMessage"
                                                        : L"Learning_AlreadyForgotMessage"));
    // The page may now be past the end, so step back one page when it was the last entry.
    if (entries_.size() == 1 && offset_ > 0) {
      offset_ = offset_ > kLearningPageSize ? offset_ - kLearningPageSize : 0;
    }
    LoadPage();
  } catch (...) {
    SetBusy(false);
  }
}

winrt::fire_and_forget LearningPane::Export() {
  const auto self = shared_from_this();
  if (busy_) co_return;
  const auto dispatcher = dispatcher_;
  try {
    ResourceLoader resources;
    azookey::ipc::ExportLearningDataRequest request;
    request.stores = CheckedStores();
    request.encrypt = encrypt_toggle_.IsOn();
    if (request.stores.empty()) {
      ShowStatus(controls::InfoBarSeverity::Warning, resources.GetString(L"Learning_ProblemTitle"),
                 resources.GetString(L"Learning_NoStoreSelected"));
      co_return;
    }
    if (!request.encrypt && !co_await Confirm(resources.GetString(L"Learning_PlaintextTitle"),
                                              resources.GetString(L"Learning_PlaintextMessage"),
                                              resources.GetString(L"Learning_ExportButton"))) {
      co_return;
    }

    winrt::Windows::Storage::Pickers::FileSavePicker picker;
    winrt::check_hresult(picker.as<IInitializeWithWindow>()->Initialize(host_.window_handle()));
    picker.SuggestedFileName(L"azookey-backup");
    picker.FileTypeChoices().Insert(L"ZIP",
                                    winrt::single_threaded_vector<winrt::hstring>({L".zip"}));
    const auto file = co_await picker.PickSaveFileAsync();
    if (!file) co_return;
    request.destination_path = winrt::to_string(file.Path());
    // The picker creates an empty placeholder, which the Host would refuse as an existing
    // destination. Only an empty file is removed; a file with content is left for the Host to
    // report as destination_exists, so nothing the user wrote is deleted here.
    std::error_code ignored;
    const std::filesystem::path chosen(file.Path().c_str());
    if (std::filesystem::is_regular_file(chosen, ignored) &&
        std::filesystem::file_size(chosen, ignored) == 0) {
      std::filesystem::remove(chosen, ignored);
    }
    if (!IsBackupArchivePath(request.destination_path)) {
      ShowStatus(controls::InfoBarSeverity::Error, resources.GetString(L"Learning_ProblemTitle"),
                 resources.GetString(L"Learning_NotZipPath"));
      co_return;
    }

    SetBusy(true);
    co_await winrt::resume_background();
    const auto result = RequestExportLearningData(DefaultSettingsIpcOptions(), request);
    co_await ResumeForeground(dispatcher);

    SetBusy(false);
    ResourceLoader after;
    if (result.status != HostCallStatus::Ok) {
      ShowStatus(controls::InfoBarSeverity::Error, after.GetString(L"Learning_ProblemTitle"),
                 Str(after, HostCallStatusResource(result.status)));
      co_return;
    }
    const auto& response = *result.response;
    if (response.status != "success") {
      ShowStatus(controls::InfoBarSeverity::Error, after.GetString(L"Learning_ProblemTitle"),
                 Str(after, LearningErrorResource(response.error.value_or(""))));
      co_return;
    }
    std::wstring summary(after.GetString(L"Learning_ExportDone"));
    summary += L"\n" + std::wstring(winrt::to_hstring(request.destination_path)) + L" (" +
               std::wstring(winrt::to_hstring(FormatByteSize(response.file_size_bytes))) + L")";
    for (const auto& item : response.items) {
      summary +=
          L"\n" + std::wstring(winrt::to_hstring(item.name)) + L": " + std::to_wstring(item.count);
    }
    backup_result_.Text(summary);
    ShowStatus(controls::InfoBarSeverity::Success, after.GetString(L"Learning_ExportDoneTitle"),
               after.GetString(L"Learning_ExportDone"));
  } catch (...) {
    SetBusy(false);
  }
}

winrt::fire_and_forget LearningPane::Import() {
  const auto self = shared_from_this();
  if (busy_) co_return;
  const auto dispatcher = dispatcher_;
  try {
    ResourceLoader resources;
    azookey::ipc::ImportLearningDataRequest request;
    request.stores = CheckedStores();
    const auto resolution = conflict_combo_.SelectedIndex();
    if (resolution >= 0 && resolution < static_cast<int>(kConflictResolutions.size())) {
      request.conflict_resolution =
          std::string(kConflictResolutions[static_cast<size_t>(resolution)]);
    }
    if (request.stores.empty()) {
      ShowStatus(controls::InfoBarSeverity::Warning, resources.GetString(L"Learning_ProblemTitle"),
                 resources.GetString(L"Learning_NoStoreSelected"));
      co_return;
    }

    winrt::Windows::Storage::Pickers::FileOpenPicker picker;
    winrt::check_hresult(picker.as<IInitializeWithWindow>()->Initialize(host_.window_handle()));
    picker.FileTypeFilter().Append(L".zip");
    const auto file = co_await picker.PickSingleFileAsync();
    if (!file) co_return;
    request.source_path = winrt::to_string(file.Path());

    auto message =
        resources.GetString(L"Learning_ImportConfirmMessage") + L"\n" + file.Path() + L"\n" +
        resources.GetString(winrt::to_hstring("LearningConflict_" + request.conflict_resolution));
    if (request.conflict_resolution == "overwrite") {
      message = message + L"\n" + resources.GetString(L"Learning_ImportOverwriteWarning");
    }
    if (!co_await Confirm(resources.GetString(L"Learning_ImportConfirmTitle"), message,
                          resources.GetString(L"Learning_ImportButton"))) {
      co_return;
    }

    SetBusy(true);
    co_await winrt::resume_background();
    const auto result = RequestImportLearningData(DefaultSettingsIpcOptions(), request);
    co_await ResumeForeground(dispatcher);

    SetBusy(false);
    ResourceLoader after;
    if (result.status != HostCallStatus::Ok) {
      ShowStatus(controls::InfoBarSeverity::Error, after.GetString(L"Learning_ProblemTitle"),
                 Str(after, HostCallStatusResource(result.status)));
      co_return;
    }
    const auto& response = *result.response;
    if (response.status != "success") {
      // "io" means the merged data is held in memory but not written to disk yet.
      ShowStatus(controls::InfoBarSeverity::Error, after.GetString(L"Learning_ProblemTitle"),
                 Str(after, LearningErrorResource(response.error.value_or(""))));
      co_return;
    }
    std::wstring summary(after.GetString(L"Learning_ImportDone"));
    for (const auto& [name, count] : response.imported_counts) {
      summary += L"\n" + std::wstring(winrt::to_hstring(name)) + L": " +
                 std::wstring(after.GetString(L"Learning_Imported")) + L" " +
                 std::to_wstring(count);
      if (const auto skipped = response.skipped_counts.find(name);
          skipped != response.skipped_counts.end()) {
        summary += L" / " + std::wstring(after.GetString(L"Learning_Skipped")) + L" " +
                   std::to_wstring(skipped->second);
      }
      if (const auto conflict = response.conflict_counts.find(name);
          conflict != response.conflict_counts.end()) {
        summary += L" / " + std::wstring(after.GetString(L"Learning_Conflicts")) + L" " +
                   std::to_wstring(conflict->second);
      }
    }
    backup_result_.Text(summary);
    ShowStatus(controls::InfoBarSeverity::Success, after.GetString(L"Learning_ImportDoneTitle"),
               after.GetString(L"Learning_ImportDone"));
  } catch (...) {
    SetBusy(false);
  }
}

}  // namespace azookey::settings
