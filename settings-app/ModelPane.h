#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "SettingsDocument.h"
#include "azookey/ipc/Payloads.h"
#include "pch.h"

namespace azookey::settings {

// The models part of the "AI 変換" pane (model-management-spec section 6.1): the model in use
// ("使うモデル", read-only, with "参照…" and "選択を外す"), the models ListModels finds with their
// validation result and a benchmark of each, and the entries of the Host's default catalog, each
// fetched on request with its progress. Choosing a model, picking a file, or a successful fetch
// changes "使うモデル"; model.selectedPath is written by the save bar.
class ModelPane : public std::enable_shared_from_this<ModelPane> {
 public:
  struct Host {
    // The chosen model.selectedPath, UTF-8; empty when none is chosen.
    std::function<std::string()> selected_path;
    // Changes the choice (an empty path clears it).
    std::function<void(const std::string&)> select_path;
    // The window the file picker of "参照…" belongs to.
    std::function<HWND()> window_handle;
  };

  void Build(winrt::Microsoft::UI::Xaml::Controls::StackPanel panel,
             winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher, Host host);
  // Called each time the pane is shown; the first call lists the models.
  void OnShown();
  // Shows the current choice again, after the window read it from settings.json.
  void ShowSelection();
  // The Host's benchmark history from settings.json (shown, never edited).
  void SetHistory(std::vector<BenchmarkHistoryEntry> history);

 private:
  // keep_message leaves the info bar as it is, so a fetch result shown just before stays.
  winrt::fire_and_forget Refresh(bool keep_message = false);
  winrt::fire_and_forget Benchmark(std::string path, std::string file_name);
  winrt::fire_and_forget FetchCatalog(std::string model_id);
  // DEV-1840: asks the Host to stop the running fetch; FetchCatalog keeps polling until it ends.
  winrt::fire_and_forget CancelCatalogFetch();
  // "参照…": a local GGUF that is not in the lists (sideload-packaging-spec section 3.7).
  winrt::fire_and_forget BrowseForModel();
  // Every way of choosing goes through here, so the lists and "使うモデル" stay in step.
  void Choose(const std::string& path);
  void RenderModels();
  void RenderCatalog();
  // Updates only the running fetch's progress bar and text, so a poll does not rebuild the rows
  // (and move the keyboard focus or Narrator off another row's button).
  void UpdateFetchProgress();
  void ShowFetchResult(const azookey::ipc::CatalogFetchResponse& response);
  void ShowBar(winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
               const winrt::hstring& title, const winrt::hstring& message);
  void RenderHistory();
  void ShowProblem(const winrt::hstring& message);
  // fetch_failed: the failure is the catalog fetch's own. Other failures leave a running fetch
  // and its poller alone.
  void RecoverFromFailure(bool fetch_failed = false);

  winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{nullptr};
  Host host_;
  winrt::Microsoft::UI::Xaml::Controls::InfoBar problem_bar_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock selected_text_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button clear_button_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button refresh_button_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel models_panel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock catalog_heading_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel catalog_panel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ProgressBar fetch_bar_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock fetch_text_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button fetch_cancel_button_{nullptr};
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
  // The Host advertises cancel_catalog_fetch, so the running fetch has a stop button.
  bool fetch_can_cancel_{false};
  // The stop was sent and the Host has not reported the fetch's end yet.
  bool cancel_requested_{false};
  // The stop was sent and the Host has not answered it yet; no fetch starts meanwhile.
  bool cancel_in_flight_{false};
  // A fetch finished while a listing was running; list again once it ends.
  bool refresh_again_{false};
};

}  // namespace azookey::settings
