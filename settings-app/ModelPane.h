#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "SettingsDocument.h"
#include "azookey/ipc/Payloads.h"
#include "pch.h"

namespace azookey::settings {

// The "モデル" pane: the models ListModels finds with their validation result, a benchmark of
// each, and the choice of model.selectedPath (model-management-spec section 6). Choosing a model
// only fills the path box of the "一般" pane; the value is saved by the common save button.
// DEV-1821: the entries of the Host's default catalog, each fetched on request with its
// progress; a fetched file is put into the path box the same way.
class ModelPane : public std::enable_shared_from_this<ModelPane> {
 public:
  struct Host {
    // The path currently in the "一般" pane's box, UTF-8.
    std::function<std::string()> selected_path;
    // Puts a path into that box.
    std::function<void(const std::string&)> select_path;
  };

  void Build(winrt::Microsoft::UI::Xaml::Controls::StackPanel panel,
             winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher, Host host);
  // Called each time the pane is shown; the first call lists the models.
  void OnShown();
  // The Host's benchmark history from settings.json (shown, never edited).
  void SetHistory(std::vector<BenchmarkHistoryEntry> history);

 private:
  // keep_message leaves the info bar as it is, so a fetch result shown just before stays.
  winrt::fire_and_forget Refresh(bool keep_message = false);
  winrt::fire_and_forget Benchmark(std::string path, std::string file_name);
  winrt::fire_and_forget FetchCatalog(std::string model_id);
  void RenderModels();
  void RenderCatalog();
  void ShowFetchResult(const azookey::ipc::CatalogFetchResponse& response);
  void ShowBar(winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
               const winrt::hstring& title, const winrt::hstring& message);
  void RenderHistory();
  void ShowProblem(const winrt::hstring& message);
  void RecoverFromFailure();

  winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{nullptr};
  Host host_;
  winrt::Microsoft::UI::Xaml::Controls::InfoBar problem_bar_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button refresh_button_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel models_panel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock catalog_heading_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel catalog_panel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock benchmark_text_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel history_panel_{nullptr};
  std::vector<azookey::ipc::ListedModel> models_;
  std::vector<azookey::ipc::CatalogModelEntry> catalog_;
  std::vector<BenchmarkHistoryEntry> history_;
  bool listed_once_{false};
  bool listing_{false};
  bool benchmarking_{false};
  bool fetching_{false};
  std::string fetching_id_;
  uint64_t fetch_received_{0};
  uint64_t fetch_total_{0};
};

}  // namespace azookey::settings
