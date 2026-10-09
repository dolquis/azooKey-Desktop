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
  winrt::fire_and_forget Refresh();
  winrt::fire_and_forget Benchmark(std::string path, std::string file_name);
  void RenderModels();
  void RenderHistory();
  void ShowProblem(const winrt::hstring& message);

  winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{nullptr};
  Host host_;
  winrt::Microsoft::UI::Xaml::Controls::InfoBar problem_bar_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button refresh_button_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel models_panel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock benchmark_text_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel history_panel_{nullptr};
  std::vector<azookey::ipc::ListedModel> models_;
  std::vector<BenchmarkHistoryEntry> history_;
  bool listed_once_{false};
  bool listing_{false};
  bool benchmarking_{false};
};

}  // namespace azookey::settings
