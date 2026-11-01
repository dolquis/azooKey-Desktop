// C++/WinRT requires the precompiled header first.
// clang-format off
#include "pch.h"
#include "ModelPane.h"
// clang-format on

#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>

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

std::string OneDecimal(double value) {
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "%.1f", value);
  return buffer;
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

controls::TextBlock Heading(const winrt::hstring& text) {
  controls::TextBlock heading;
  heading.Text(text);
  heading.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
  heading.Margin(xaml::ThicknessHelper::FromLengths(0, 8, 0, 0));
  return heading;
}

void SetId(xaml::DependencyObject const& object, const std::wstring& id) {
  xaml::Automation::AutomationProperties::SetAutomationId(object, id);
}

winrt::hstring StateText(const ResourceLoader& resources, ModelEntryState state) {
  switch (state) {
    case ModelEntryState::Loaded:
      return resources.GetString(L"Models_State_Loaded");
    case ModelEntryState::NotLoaded:
      return resources.GetString(L"Models_State_NotLoaded");
    case ModelEntryState::LoadFailed:
      return resources.GetString(L"Models_State_LoadFailed");
    case ModelEntryState::Invalid:
      break;
  }
  return resources.GetString(L"Models_State_Invalid");
}

std::string MetadataText(const azookey::ipc::ListedModelMetadata& metadata) {
  std::string text;
  const auto append = [&text](const std::string& part) {
    if (part.empty()) return;
    if (!text.empty()) text += " / ";
    text += part;
  };
  append(metadata.model_family);
  append(metadata.quantization);
  if (metadata.n_params != 0) {
    append(OneDecimal(static_cast<double>(metadata.n_params) / 1e6) + " M params");
  }
  return text;
}

}  // namespace

void ModelPane::Build(controls::StackPanel panel,
                      winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher, Host host) {
  dispatcher_ = std::move(dispatcher);
  host_ = std::move(host);
  ResourceLoader resources;

  controls::TextBlock title;
  title.Text(resources.GetString(L"Models_Title"));
  title.FontSize(20);
  title.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
  panel.Children().Append(title);
  panel.Children().Append(Block(resources.GetString(L"Models_Description"), true));

  problem_bar_ = controls::InfoBar();
  problem_bar_.IsOpen(false);
  problem_bar_.IsClosable(true);
  SetId(problem_bar_, L"ModelsStatusInfoBar");
  panel.Children().Append(problem_bar_);

  refresh_button_ = controls::Button();
  refresh_button_.Content(winrt::box_value(resources.GetString(L"Models_RefreshButton")));
  SetId(refresh_button_, L"ModelsRefreshButton");
  refresh_button_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (const auto self = weak.lock()) self->Refresh();
  });
  panel.Children().Append(refresh_button_);

  models_panel_ = controls::StackPanel();
  models_panel_.Spacing(12);
  SetId(models_panel_, L"ModelsList");
  panel.Children().Append(models_panel_);

  // Filled from the ListModels catalog; stays hidden for a Host that sends none.
  catalog_heading_ = Heading(resources.GetString(L"Models_Catalog_Heading"));
  catalog_heading_.Visibility(xaml::Visibility::Collapsed);
  panel.Children().Append(catalog_heading_);
  catalog_panel_ = controls::StackPanel();
  catalog_panel_.Spacing(12);
  SetId(catalog_panel_, L"ModelsCatalog");
  panel.Children().Append(catalog_panel_);

  panel.Children().Append(Heading(resources.GetString(L"Models_BenchmarkHeading")));
  benchmark_text_ = Block(resources.GetString(L"Models_BenchmarkNone"), true);
  SetId(benchmark_text_, L"ModelsBenchmarkResult");
  panel.Children().Append(benchmark_text_);

  panel.Children().Append(Heading(resources.GetString(L"Models_HistoryHeading")));
  history_panel_ = controls::StackPanel();
  history_panel_.Spacing(4);
  SetId(history_panel_, L"ModelsHistory");
  panel.Children().Append(history_panel_);
  RenderHistory();
}

void ModelPane::OnShown() {
  if (!listed_once_ && !listing_) Refresh();
}

void ModelPane::SetHistory(std::vector<BenchmarkHistoryEntry> history) {
  history_ = std::move(history);
  if (history_panel_) RenderHistory();
}

void ModelPane::RecoverFromFailure(bool fetch_failed) {
  // Reached from catch blocks that may run on a background thread, so the flags and controls
  // are only touched from the UI thread.
  dispatcher_.TryEnqueue([self = shared_from_this(), fetch_failed] {
    self->listing_ = false;
    self->benchmarking_ = false;
    if (fetch_failed) {
      self->fetching_ = false;
      self->fetching_id_.clear();
    }
    self->refresh_button_.IsEnabled(true);
    ResourceLoader resources;
    self->benchmark_text_.Text(resources.GetString(L"Models_BenchmarkNone"));
    self->RenderModels();
    self->RenderCatalog();
    self->ShowProblem(resources.GetString(L"Models_UnexpectedError"));
  });
}

void ModelPane::ShowProblem(const winrt::hstring& message) {
  ResourceLoader resources;
  ShowBar(controls::InfoBarSeverity::Error, resources.GetString(L"Models_ProblemTitle"), message);
}

void ModelPane::ShowBar(controls::InfoBarSeverity severity, const winrt::hstring& title,
                        const winrt::hstring& message) {
  problem_bar_.Severity(severity);
  problem_bar_.Title(title);
  problem_bar_.Message(message);
  problem_bar_.IsOpen(true);
}

winrt::fire_and_forget ModelPane::Refresh(bool keep_message) {
  const auto self = shared_from_this();
  if (listing_) co_return;
  listing_ = true;
  refresh_button_.IsEnabled(false);
  if (!keep_message) problem_bar_.IsOpen(false);
  const auto dispatcher = dispatcher_;
  try {
    co_await winrt::resume_background();
    azookey::ipc::ListModelsRequest request;
    request.include_catalog = true;
    const auto result = RequestListModels(DefaultSettingsIpcOptions(), request);
    co_await ResumeForeground(dispatcher);

    listing_ = false;
    refresh_button_.IsEnabled(true);
    ResourceLoader resources;
    if (result.status != HostCallStatus::Ok) {
      ShowProblem(Str(resources, HostCallStatusResource(result.status)));
    } else if (!result.response->ok) {
      ShowProblem(Str(resources, ModelErrorResource(result.response->error.value_or(""))));
    } else {
      listed_once_ = true;
      models_ = result.response->models;
      catalog_ = result.response->catalog.value_or(std::vector<azookey::ipc::CatalogModelEntry>{});
      RenderModels();
      RenderCatalog();
    }
    if (refresh_again_) {
      refresh_again_ = false;
      Refresh(/*keep_message=*/true);
    }
  } catch (...) {
    RecoverFromFailure();
  }
}

void ModelPane::RenderModels() {
  ResourceLoader resources;
  models_panel_.Children().Clear();
  if (models_.empty()) {
    models_panel_.Children().Append(Block(resources.GetString(L"Models_Empty"), true));
    return;
  }
  const auto selected = host_.selected_path ? host_.selected_path() : std::string();
  for (const auto& model : models_) {
    const bool selectable = IsSelectableModel(model);
    const bool is_selected = SameModelPath(model.path, selected);
    const auto state = ClassifyModel(model);

    controls::StackPanel row;
    row.Spacing(4);
    auto name = winrt::to_hstring(model.file_name);
    if (is_selected) name = name + resources.GetString(L"Models_SelectedSuffix");
    auto name_block = Block(name);
    name_block.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
    row.Children().Append(name_block);

    std::string detail = model.format + " / " + FormatByteSize(model.size_bytes);
    if (const auto metadata = MetadataText(model.metadata); !metadata.empty()) {
      detail += " / " + metadata;
    }
    row.Children().Append(Block(winrt::to_hstring(detail), true));

    auto state_text = StateText(resources, state);
    if (!model.last_error.empty())
      state_text = state_text + L" (" + winrt::to_hstring(model.last_error) + L")";
    row.Children().Append(Block(state_text, true));
    if (!model.sha256.empty()) {
      row.Children().Append(Block(winrt::to_hstring("SHA-256: " + model.sha256), true));
    }

    controls::StackPanel buttons;
    buttons.Orientation(controls::Orientation::Horizontal);
    buttons.Spacing(8);
    controls::Button select_button;
    select_button.Content(winrt::box_value(resources.GetString(L"Models_SelectButton")));
    select_button.IsEnabled(selectable && !is_selected);
    SetId(select_button, L"ModelSelectButton_" + std::wstring(winrt::to_hstring(model.file_name)));
    select_button.Click([weak = weak_from_this(), path = model.path](auto const&, auto const&) {
      const auto self = weak.lock();
      if (!self) return;
      self->host_.select_path(path);
      ResourceLoader strings;
      self->problem_bar_.Severity(controls::InfoBarSeverity::Informational);
      self->problem_bar_.Title(strings.GetString(L"Models_SelectedTitle"));
      self->problem_bar_.Message(strings.GetString(L"Models_SelectedMessage"));
      self->problem_bar_.IsOpen(true);
      self->RenderModels();
    });
    buttons.Children().Append(select_button);

    controls::Button benchmark_button;
    benchmark_button.Content(winrt::box_value(resources.GetString(L"Models_BenchmarkButton")));
    benchmark_button.IsEnabled(selectable && !benchmarking_);
    SetId(benchmark_button,
          L"ModelBenchmarkButton_" + std::wstring(winrt::to_hstring(model.file_name)));
    benchmark_button.Click([weak = weak_from_this(), path = model.path,
                            file_name = model.file_name](auto const&, auto const&) {
      if (const auto self = weak.lock()) self->Benchmark(path, file_name);
    });
    buttons.Children().Append(benchmark_button);
    row.Children().Append(buttons);

    if (!selectable) {
      row.Children().Append(Block(resources.GetString(L"Models_NotSelectable"), true));
    }
    models_panel_.Children().Append(row);
  }
}

winrt::fire_and_forget ModelPane::Benchmark(std::string path, std::string file_name) {
  const auto self = shared_from_this();
  if (benchmarking_) co_return;
  benchmarking_ = true;
  {
    ResourceLoader resources;
    benchmark_text_.Text(winrt::to_hstring(file_name) + L"\n" +
                         resources.GetString(L"Models_BenchmarkRunning"));
  }
  RenderModels();
  problem_bar_.IsOpen(false);
  const auto dispatcher = dispatcher_;
  try {
    co_await winrt::resume_background();
    azookey::ipc::BenchmarkModelRequest request;
    request.path = path;
    request.backend = "cpu";
    const auto result = RequestBenchmarkModel(DefaultSettingsIpcOptions(), request);
    std::optional<SettingsDocumentResult> history;
    if (result.status == HostCallStatus::Ok) {
      if (const auto settings_path = DefaultSettingsPath()) {
        history = LoadSettingsDocument(*settings_path);
      }
    }
    co_await ResumeForeground(dispatcher);

    benchmarking_ = false;
    RenderModels();
    ResourceLoader resources;
    if (result.status != HostCallStatus::Ok) {
      benchmark_text_.Text(resources.GetString(L"Models_BenchmarkNone"));
      ShowProblem(Str(resources, HostCallStatusResource(result.status)));
      co_return;
    }
    if (history && history->status == SettingsDocumentStatus::Loaded) {
      SetHistory(std::move(history->settings.benchmark_history));
    }
    const auto& r = *result.response;
    std::wstring text(winrt::to_hstring(file_name));
    const auto line = [&text, &resources](const wchar_t* label, const std::string& value) {
      text += L"\n" + std::wstring(resources.GetString(label)) + L": " +
              std::wstring(winrt::to_hstring(value));
    };
    line(L"Models_Bench_Status", r.status);
    if (r.error) {
      line(L"Models_Bench_Error", winrt::to_string(Str(resources, ModelErrorResource(*r.error))));
    }
    line(L"Models_Bench_Backend", r.backend);
    line(L"Models_Bench_Latency", OneDecimal(r.p50_ms) + " / " + OneDecimal(r.p95_ms) + " / " +
                                      OneDecimal(r.p99_ms) + " ms");
    line(L"Models_Bench_Load", OneDecimal(r.load_ms) + " ms");
    line(L"Models_Bench_Memory",
         OneDecimal(r.rss_mb) + " MB (VRAM " +
             (r.vram_mb ? OneDecimal(*r.vram_mb) + " MB" : std::string("-")) + ")");
    line(L"Models_Bench_Iterations", std::to_string(r.iterations_completed));
    benchmark_text_.Text(text);
  } catch (...) {
    RecoverFromFailure();
  }
}

void ModelPane::UpdateFetchProgress() {
  if (!fetch_bar_ || !fetch_text_) return;
  ResourceLoader resources;
  const auto percent = ProgressPercent(fetch_received_, fetch_total_);
  fetch_bar_.IsIndeterminate(fetch_total_ == 0);
  fetch_bar_.Value(percent);
  std::string progress = FormatByteSize(fetch_received_);
  if (fetch_total_ != 0) {
    progress =
        std::to_string(percent) + "% (" + progress + " / " + FormatByteSize(fetch_total_) + ")";
  }
  fetch_text_.Text(resources.GetString(L"Models_Catalog_Fetching") + L" " +
                   winrt::to_hstring(progress));
}

void ModelPane::RenderCatalog() {
  ResourceLoader resources;
  catalog_panel_.Children().Clear();
  fetch_bar_ = nullptr;
  fetch_text_ = nullptr;
  catalog_heading_.Visibility(catalog_.empty() ? xaml::Visibility::Collapsed
                                               : xaml::Visibility::Visible);
  const auto selected = host_.selected_path ? host_.selected_path() : std::string();
  for (const auto& entry : catalog_) {
    const bool verified = entry.state == "verified";
    const bool is_selected = verified && SameModelPath(entry.path, selected);
    const auto id = std::wstring(winrt::to_hstring(entry.id));

    controls::StackPanel row;
    row.Spacing(4);
    auto name = winrt::to_hstring(entry.display_name.empty() ? entry.id : entry.display_name);
    if (entry.is_default) name = name + resources.GetString(L"Models_Catalog_DefaultSuffix");
    if (is_selected) name = name + resources.GetString(L"Models_SelectedSuffix");
    auto name_block = Block(name);
    name_block.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
    row.Children().Append(name_block);
    std::string detail = entry.file_name;
    if (entry.size_bytes != 0) detail += " / " + FormatByteSize(entry.size_bytes);
    if (!entry.prompt_format.empty()) detail += " / " + entry.prompt_format;
    row.Children().Append(Block(winrt::to_hstring(detail), true));
    row.Children().Append(Block(Str(resources, CatalogEntryStateResource(entry.state)), true));

    if (fetching_ && fetching_id_ == entry.id) {
      fetch_bar_ = controls::ProgressBar();
      SetId(fetch_bar_, L"CatalogFetchProgress_" + id);
      fetch_bar_.Maximum(100);
      row.Children().Append(fetch_bar_);
      fetch_text_ = Block(L"", true);
      row.Children().Append(fetch_text_);
      UpdateFetchProgress();
    }

    controls::StackPanel buttons;
    buttons.Orientation(controls::Orientation::Horizontal);
    buttons.Spacing(8);
    controls::Button fetch_button;
    fetch_button.Content(winrt::box_value(resources.GetString(L"Models_Catalog_FetchButton")));
    // A same-named file with another SHA-256 is never replaced, so it is not offered.
    fetch_button.IsEnabled(entry.fetchable && entry.state == "missing" && !fetching_);
    SetId(fetch_button, L"CatalogFetchButton_" + id);
    fetch_button.Click([weak = weak_from_this(), model_id = entry.id](auto const&, auto const&) {
      if (const auto self = weak.lock()) self->FetchCatalog(model_id);
    });
    buttons.Children().Append(fetch_button);

    controls::Button select_button;
    select_button.Content(winrt::box_value(resources.GetString(L"Models_SelectButton")));
    select_button.IsEnabled(verified && !is_selected);
    SetId(select_button, L"CatalogSelectButton_" + id);
    select_button.Click([weak = weak_from_this(), path = entry.path](auto const&, auto const&) {
      const auto self = weak.lock();
      if (!self) return;
      self->host_.select_path(path);
      ResourceLoader strings;
      self->ShowBar(controls::InfoBarSeverity::Informational,
                    strings.GetString(L"Models_SelectedTitle"),
                    strings.GetString(L"Models_SelectedMessage"));
      self->RenderModels();
      self->RenderCatalog();
    });
    buttons.Children().Append(select_button);
    row.Children().Append(buttons);
    if (!entry.fetchable && entry.state == "missing") {
      row.Children().Append(Block(resources.GetString(L"Models_Catalog_NotFetchableNote"), true));
    }
    catalog_panel_.Children().Append(row);
  }
}

// DEV-1821 (model-management-spec section 4.4): starts the fetch, then asks for its progress
// until it is done. Only a stall ends the wait early: a large model may take long.
winrt::fire_and_forget ModelPane::FetchCatalog(std::string model_id) {
  const auto self = shared_from_this();
  if (fetching_) co_return;
  fetching_ = true;
  fetching_id_ = model_id;
  fetch_received_ = 0;
  fetch_total_ = 0;
  problem_bar_.IsOpen(false);
  RenderCatalog();
  const auto dispatcher = dispatcher_;
  try {
    co_await winrt::resume_background();
    const auto options = DefaultSettingsIpcOptions();
    azookey::ipc::FetchCatalogModelRequest request;
    request.model_id = model_id;
    const auto started = RequestFetchCatalogModel(options, request);
    HostCallStatus call_status = started.status;
    std::optional<azookey::ipc::CatalogFetchResponse> last = started.response;
    // A fetch of the same entry the Host is already running (from another window) is followed
    // as if this one had started it.
    const bool follow = call_status == HostCallStatus::Ok &&
                        (last->ok || (last->error == "busy" && last->model_id == model_id));
    bool stalled = false;
    // A poll that gets no answer is retried: only a stall (no new bytes, including polls that
    // fail for that long) ends the wait.
    HostCallStatus poll_status = HostCallStatus::Ok;
    CatalogFetchStallWatch watch(CatalogFetchStallWatch::Clock::now());
    while (follow && last->state == "running" && last->model_id == model_id) {
      co_await ResumeForeground(dispatcher);
      fetch_received_ = last->received_bytes;
      fetch_total_ = last->total_bytes;
      UpdateFetchProgress();
      if (watch.Stalled(last->received_bytes, CatalogFetchStallWatch::Clock::now())) {
        stalled = true;
        break;
      }
      co_await winrt::resume_after(std::chrono::milliseconds(500));
      auto poll = RequestQueryCatalogFetch(options);
      poll_status = poll.status;
      if (poll_status == HostCallStatus::Ok) last = std::move(poll.response);
    }
    co_await ResumeForeground(dispatcher);

    fetching_ = false;
    fetching_id_.clear();
    ResourceLoader resources;
    const auto failed_title = resources.GetString(L"Models_Catalog_FailedTitle");
    if (call_status != HostCallStatus::Ok) {
      ShowBar(controls::InfoBarSeverity::Error, failed_title,
              Str(resources, HostCallStatusResource(call_status)));
    } else if (!follow) {
      ShowBar(controls::InfoBarSeverity::Error, failed_title,
              Str(resources, CatalogFetchErrorResource(last->error.value_or(""))));
    } else if (stalled) {
      // The last poll's own failure says more than the stall when the Host stopped answering.
      ShowBar(controls::InfoBarSeverity::Warning, failed_title,
              poll_status == HostCallStatus::Ok
                  ? resources.GetString(L"Models_Catalog_Stalled")
                  : Str(resources, HostCallStatusResource(poll_status)));
    } else if (last->state == "done" && last->model_id == model_id) {
      ShowFetchResult(*last);
    } else if (last->state == "idle") {
      // The Host restarted while fetching; the .part it left lets the next fetch resume.
      ShowBar(controls::InfoBarSeverity::Warning, failed_title,
              resources.GetString(L"Models_Catalog_Interrupted"));
    } else {
      // The Host ran another entry's fetch meanwhile; this one's outcome is not known.
      ShowBar(controls::InfoBarSeverity::Warning, failed_title,
              Str(resources, CatalogFetchErrorResource("busy")));
    }
    RenderCatalog();
    // A listing already running may have read the catalog before the fetch ended.
    if (listing_) {
      refresh_again_ = true;
    } else {
      Refresh(/*keep_message=*/true);
    }
  } catch (...) {
    RecoverFromFailure(/*fetch_failed=*/true);
  }
}

// sideload-packaging-spec section 1.6.1: the fetch's SHA-256 match and GGUF checks are the
// probe, and the path box plus the common save button are the commit.
void ModelPane::ShowFetchResult(const azookey::ipc::CatalogFetchResponse& response) {
  ResourceLoader resources;
  const auto result = Str(resources, CatalogFetchResultResource(response));
  if (CatalogFetchSucceeded(response)) {
    host_.select_path(response.path);
    ShowBar(controls::InfoBarSeverity::Success, resources.GetString(L"Models_Catalog_FetchedTitle"),
            result + L"\n" + resources.GetString(L"Models_SelectedMessage"));
    return;
  }
  const bool failed = response.status == "failed";
  ShowBar(failed ? controls::InfoBarSeverity::Error : controls::InfoBarSeverity::Warning,
          resources.GetString(L"Models_Catalog_FailedTitle"), result);
}

void ModelPane::RenderHistory() {
  ResourceLoader resources;
  history_panel_.Children().Clear();
  if (history_.empty()) {
    history_panel_.Children().Append(Block(resources.GetString(L"Models_HistoryEmpty"), true));
    return;
  }
  for (const auto& entry : history_) {
    std::string line;
    const auto append = [&line](const std::string& part) {
      if (part.empty()) return;
      if (!line.empty()) line += " / ";
      line += part;
    };
    append(entry.path);
    append(entry.completed_at);
    append(entry.backend);
    append(entry.status);
    if (entry.p50_ms || entry.p95_ms || entry.p99_ms) {
      const auto ms = [](const std::optional<double>& value) {
        return value ? OneDecimal(*value) : std::string("-");
      };
      append("p50/p95/p99 " + ms(entry.p50_ms) + "/" + ms(entry.p95_ms) + "/" + ms(entry.p99_ms) +
             " ms");
    }
    if (entry.load_ms) append("load " + OneDecimal(*entry.load_ms) + " ms");
    if (entry.rss_mb) append("RSS " + OneDecimal(*entry.rss_mb) + " MB");
    if (entry.vram_mb) append("VRAM " + OneDecimal(*entry.vram_mb) + " MB");
    if (entry.iterations_completed) append(std::to_string(*entry.iterations_completed));
    if (entry.error) append(winrt::to_string(Str(resources, ModelErrorResource(*entry.error))));
    if (line.empty()) continue;
    history_panel_.Children().Append(Block(winrt::to_hstring(line), true));
  }
}

}  // namespace azookey::settings
