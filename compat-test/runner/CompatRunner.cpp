#include <Ole2.h>
#include <UIAutomation.h>

#include "runner/CompatTypes.h"

#ifdef GetObject
#undef GetObject
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "runner/ReportWriter.h"
#include "runner/ScreenshotCapture.h"
#include "runner/TargetConfigLoader.h"
#include "runner/WindowOwnership.h"

namespace azookey::compat_test {
namespace {

template <typename T>
void SafeRelease(T*& value) {
  if (value) {
    value->Release();
    value = nullptr;
  }
}

bool IsConfiguredWindow(HWND window, const TargetConfig& target) {
  if (!IsWindowVisible(window)) return false;
  wchar_t class_name[256]{};
  if (GetClassNameW(window, class_name, static_cast<int>(std::size(class_name))) <= 0) return false;
  return std::find(target.window_classes.begin(), target.window_classes.end(), class_name) !=
         target.window_classes.end();
}

std::set<HWND> SnapshotTargetWindows(const TargetConfig& target) {
  struct Data {
    const TargetConfig* target;
    std::set<HWND>* windows;
  } data{&target, nullptr};
  std::set<HWND> windows;
  data.windows = &windows;
  EnumWindows(
      [](HWND window, LPARAM param) -> BOOL {
        auto* data = reinterpret_cast<Data*>(param);
        if (IsConfiguredWindow(window, *data->target)) data->windows->insert(window);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&data));
  return windows;
}

bool TargetExecutableMatches(HWND window, const TargetConfig& target, DWORD launched_process_id) {
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  if (process_id == launched_process_id) return true;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  wchar_t image_path[32768]{};
  DWORD image_path_size = static_cast<DWORD>(std::size(image_path));
  const bool image_matches =
      process && QueryFullProcessImageNameW(process, 0, image_path, &image_path_size) &&
      _wcsicmp(std::filesystem::path(image_path).filename().c_str(),
               std::filesystem::path(target.executable).filename().c_str()) == 0;
  if (process) CloseHandle(process);
  return image_matches;
}

bool ContainsCaseInsensitive(std::wstring_view text, std::wstring_view needle) {
  if (needle.empty() || needle.size() > text.size()) return false;
  return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                     [](wchar_t left, wchar_t right) {
                       return std::towlower(left) == std::towlower(right);
                     }) != text.end();
}

bool TemporaryDocumentIsSelected(IUIAutomation* automation, HWND window,
                                 const std::filesystem::path& temporary_document) {
  if (!automation || temporary_document.empty()) return false;
  const auto filename = temporary_document.filename().wstring();
  const auto stem = temporary_document.stem().wstring();
  const auto matches_document = [&](std::wstring_view name) {
    return ContainsCaseInsensitive(name, filename) || ContainsCaseInsensitive(name, stem);
  };

  IUIAutomationElement* root = nullptr;
  if (SUCCEEDED(automation->ElementFromHandle(window, &root)) && root) {
    VARIANT type_value;
    VariantInit(&type_value);
    type_value.vt = VT_I4;
    type_value.lVal = UIA_TabItemControlTypeId;
    IUIAutomationCondition* condition = nullptr;
    IUIAutomationElementArray* tabs = nullptr;
    if (SUCCEEDED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type_value,
                                                      &condition)) &&
        condition && SUCCEEDED(root->FindAll(TreeScope_Descendants, condition, &tabs)) && tabs) {
      int length = 0;
      tabs->get_Length(&length);
      for (int index = 0; index < length; ++index) {
        IUIAutomationElement* tab = nullptr;
        if (FAILED(tabs->GetElement(index, &tab)) || !tab) continue;
        BSTR name = nullptr;
        tab->get_CurrentName(&name);
        IUnknown* unknown = nullptr;
        IUIAutomationSelectionItemPattern* selection = nullptr;
        BOOL selected = FALSE;
        const bool is_selected_document =
            name && matches_document(std::wstring_view(name, SysStringLen(name))) &&
            SUCCEEDED(tab->GetCurrentPattern(UIA_SelectionItemPatternId, &unknown)) && unknown &&
            SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&selection))) && selection &&
            SUCCEEDED(selection->get_CurrentIsSelected(&selected)) && selected;
        if (name) SysFreeString(name);
        SafeRelease(selection);
        SafeRelease(unknown);
        SafeRelease(tab);
        if (is_selected_document) {
          SafeRelease(tabs);
          SafeRelease(condition);
          SafeRelease(root);
          return true;
        }
      }
    }
    SafeRelease(tabs);
    SafeRelease(condition);
    SafeRelease(root);
  }

  const int title_length = GetWindowTextLengthW(window);
  if (title_length <= 0) return false;
  std::vector<wchar_t> title(static_cast<size_t>(title_length) + 1);
  if (GetWindowTextW(window, title.data(), static_cast<int>(title.size())) <= 0) return false;
  return matches_document(title.data());
}

std::vector<TargetWindowObservation> ObserveTargetWindows(
    const TargetConfig& target, const std::set<HWND>& existing, DWORD launched_process_id,
    IUIAutomation* automation, const std::filesystem::path& temporary_document) {
  struct Data {
    const TargetConfig* target;
    const std::set<HWND>* existing;
    DWORD launched_process_id;
    IUIAutomation* automation;
    const std::filesystem::path* temporary_document;
    std::vector<TargetWindowObservation>* observations;
  } data{&target, &existing, launched_process_id, automation, &temporary_document, nullptr};
  std::vector<TargetWindowObservation> observations;
  data.observations = &observations;
  EnumWindows(
      [](HWND window, LPARAM param) -> BOOL {
        auto* data = reinterpret_cast<Data*>(param);
        if (!IsConfiguredWindow(window, *data->target)) return TRUE;
        DWORD process_id = 0;
        GetWindowThreadProcessId(window, &process_id);
        const bool existed_before = data->existing->contains(window);
        data->observations->push_back(
            {.window_id = reinterpret_cast<std::uintptr_t>(window),
             .process_id = process_id,
             .existed_before = existed_before,
             .executable_matches =
                 TargetExecutableMatches(window, *data->target, data->launched_process_id),
             .temporary_document_selected =
                 existed_before && data->target->allow_reused_window_for_temporary_document &&
                 TemporaryDocumentIsSelected(data->automation, window, *data->temporary_document)});
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&data));
  return observations;
}

bool FindFirstVisibleDescendant(IUIAutomationElement* root, IUIAutomationCondition* condition,
                                IUIAutomationElement** result) {
  IUIAutomationElementArray* matches = nullptr;
  if (FAILED(root->FindAll(TreeScope_Descendants, condition, &matches)) || !matches) return false;
  int length = 0;
  matches->get_Length(&length);
  for (int index = 0; index < length; ++index) {
    IUIAutomationElement* element = nullptr;
    if (FAILED(matches->GetElement(index, &element)) || !element) continue;
    BOOL offscreen = TRUE;
    if (SUCCEEDED(element->get_CurrentIsOffscreen(&offscreen)) && !offscreen) {
      *result = element;
      SafeRelease(matches);
      return true;
    }
    SafeRelease(element);
  }
  SafeRelease(matches);
  return false;
}

std::wstring QuoteArgument(std::wstring_view value) {
  std::wstring result = L"\"";
  for (const wchar_t ch : value) {
    if (ch == L'"') result += L'\\';
    result += ch;
  }
  result += L'"';
  return result;
}

std::optional<std::wstring> ReadElementText(IUIAutomationElement* element) {
  const auto normalize = [](std::wstring value) {
    while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n')) value.pop_back();
    return value;
  };
  std::optional<std::wstring> empty_value;
  IUnknown* unknown = nullptr;
  if (SUCCEEDED(element->GetCurrentPattern(UIA_ValuePatternId, &unknown)) && unknown) {
    IUIAutomationValuePattern* value_pattern = nullptr;
    if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&value_pattern))) && value_pattern) {
      BSTR value = nullptr;
      const HRESULT hr = value_pattern->get_CurrentValue(&value);
      std::wstring result =
          SUCCEEDED(hr) && value ? std::wstring(value, SysStringLen(value)) : std::wstring{};
      if (value) SysFreeString(value);
      SafeRelease(value_pattern);
      SafeRelease(unknown);
      if (SUCCEEDED(hr)) {
        result = normalize(std::move(result));
        if (!result.empty()) return result;
        empty_value = std::move(result);
      }
    }
    SafeRelease(unknown);
  }

  if (SUCCEEDED(element->GetCurrentPattern(UIA_TextPatternId, &unknown)) && unknown) {
    IUIAutomationTextPattern* text_pattern = nullptr;
    if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&text_pattern))) && text_pattern) {
      IUIAutomationTextRange* range = nullptr;
      BSTR value = nullptr;
      HRESULT hr = text_pattern->get_DocumentRange(&range);
      if (SUCCEEDED(hr) && range) hr = range->GetText(-1, &value);
      std::wstring result =
          SUCCEEDED(hr) && value ? std::wstring(value, SysStringLen(value)) : std::wstring{};
      if (value) SysFreeString(value);
      SafeRelease(range);
      SafeRelease(text_pattern);
      SafeRelease(unknown);
      if (SUCCEEDED(hr)) return normalize(std::move(result));
    }
    SafeRelease(unknown);
  }
  return empty_value;
}

std::filesystem::path FailureDirectory(const std::filesystem::path& output,
                                       const CaseResult& result) {
  return output / "failures" /
         (result.id + "_" + (result.status == ResultStatus::Fail ? "fail" : "failing-skip"));
}

}  // namespace

AutomationSession::AutomationSession(TargetConfig target) : target_(std::move(target)) {}

AutomationSession::~AutomationSession() {
  CloseLaunchedWindow();
  SafeRelease(editor_);
  SafeRelease(automation_);
  if (process_info_.hThread) CloseHandle(process_info_.hThread);
  if (process_info_.hProcess) CloseHandle(process_info_.hProcess);
  if (!temporary_document_.empty()) {
    std::error_code ec;
    std::filesystem::remove(temporary_document_, ec);
  }
}

bool AutomationSession::Start(std::string* reason_code) {
  const auto existing = SnapshotTargetWindows(target_);
  const auto executable = ResolveExecutable(target_.executable);
  std::cout << "Executable resolution (" << target_.id
            << "): " << ExecutableResolutionSourceName(executable.source) << "\n";
  std::wstring command = QuoteArgument(executable.path.wstring());
  for (const auto& argument : target_.arguments) command += L" " + QuoteArgument(argument);
  if (target_.use_temporary_document) {
    temporary_document_ = CreateTemporaryDocument(target_);
    if (temporary_document_.empty()) {
      if (reason_code) *reason_code = "temporary-document-create-failed";
      return false;
    }
    command += L" " + QuoteArgument(temporary_document_.wstring());
  }
  std::vector<wchar_t> writable(command.begin(), command.end());
  writable.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  if (!CreateProcessW(nullptr, writable.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                      &startup, &process_info_)) {
    if (reason_code) *reason_code = "target-launch-failed";
    return false;
  }
  owns_launched_process_ = true;
  if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&automation_)))) {
    if (reason_code) *reason_code = "uia-initialization-failed";
    return false;
  }
  WaitForInputIdle(process_info_.hProcess, 5000);
  TargetSurfaceSelection selection;
  for (int attempt = 0; attempt < 100 && !window_; ++attempt) {
    const auto observations = ObserveTargetWindows(target_, existing, process_info_.dwProcessId,
                                                   automation_, temporary_document_);
    selection = SelectTargetSurface(observations, process_info_.dwProcessId,
                                    target_.allow_reused_window_for_temporary_document);
    if (selection.ownership != TargetSurfaceOwnership::None) {
      window_ = reinterpret_cast<HWND>(selection.window_id);
      owns_window_ = selection.ownership == TargetSurfaceOwnership::Window;
      owns_document_tab_ = selection.ownership == TargetSurfaceOwnership::DocumentTab;
    }
    if (!window_) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!window_) {
    if (reason_code) *reason_code = TargetSurfaceFailureReason(selection.failure);
    return false;
  }
  constexpr auto kEditorDiscoveryTimeout = std::chrono::seconds(15);
  const auto editor_discovery_started = std::chrono::steady_clock::now();
  const auto editor_discovery_deadline = editor_discovery_started + kEditorDiscoveryTimeout;
  do {
    if (FindEditorElement()) break;
    if (std::chrono::steady_clock::now() >= editor_discovery_deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  } while (!editor_);
  editor_discovery_duration_ms_ =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - editor_discovery_started)
                                .count());
  if (!editor_) {
    if (reason_code) *reason_code = "editor-element-not-found";
    return false;
  }
  if (!FocusEditor()) {
    if (reason_code) *reason_code = "editor-focus-failed";
    return false;
  }
  return true;
}

bool AutomationSession::FindEditorElement() {
  IUIAutomationElement* root = nullptr;
  if (FAILED(automation_->ElementFromHandle(window_, &root)) || !root) return false;
  const std::array<CONTROLTYPEID, 2> control_types =
      target_.editor_control_type == "edit"
          ? std::array<CONTROLTYPEID, 2>{UIA_EditControlTypeId, UIA_DocumentControlTypeId}
          : std::array<CONTROLTYPEID, 2>{UIA_DocumentControlTypeId, UIA_EditControlTypeId};
  for (const CONTROLTYPEID control_type : control_types) {
    VARIANT type_value;
    VariantInit(&type_value);
    type_value.vt = VT_I4;
    type_value.lVal = control_type;
    IUIAutomationCondition* type_condition = nullptr;
    if (FAILED(automation_->CreatePropertyCondition(UIA_ControlTypePropertyId, type_value,
                                                    &type_condition)) ||
        !type_condition) {
      continue;
    }

    IUIAutomationCondition* name_condition = nullptr;
    IUIAutomationCondition* combined_condition = nullptr;
    IUIAutomationCondition* search_condition = type_condition;
    if (!target_.editor_name.empty()) {
      VARIANT name_value;
      VariantInit(&name_value);
      name_value.vt = VT_BSTR;
      name_value.bstrVal = SysAllocString(target_.editor_name.c_str());
      const bool name_created = name_value.bstrVal &&
                                SUCCEEDED(automation_->CreatePropertyCondition(
                                    UIA_NamePropertyId, name_value, &name_condition)) &&
                                name_condition;
      VariantClear(&name_value);
      if (!name_created ||
          FAILED(automation_->CreateAndCondition(type_condition, name_condition,
                                                 &combined_condition)) ||
          !combined_condition) {
        SafeRelease(name_condition);
        SafeRelease(type_condition);
        continue;
      }
      search_condition = combined_condition;
    }

    FindFirstVisibleDescendant(root, search_condition, &editor_);
    SafeRelease(combined_condition);
    SafeRelease(name_condition);
    SafeRelease(type_condition);
    if (editor_) break;
  }
  if (!editor_ && target_.editor_name.empty() && !target_.edit_control_class.empty()) {
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(target_.edit_control_class.c_str());
    IUIAutomationCondition* condition = nullptr;
    if (value.bstrVal &&
        SUCCEEDED(
            automation_->CreatePropertyCondition(UIA_ClassNamePropertyId, value, &condition)) &&
        condition) {
      FindFirstVisibleDescendant(root, condition, &editor_);
      SafeRelease(condition);
    }
    VariantClear(&value);
  }
  SafeRelease(root);
  return editor_ != nullptr;
}

bool AutomationSession::FocusEditor() {
  if (!window_ || !editor_) {
    focus_lost_ = true;
    return false;
  }
  ShowWindow(window_, SW_RESTORE);
  SetForegroundWindow(window_);
  editor_->SetFocus();
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (IsTargetForeground()) {
      focus_lost_ = false;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  focus_lost_ = true;
  return false;
}

bool AutomationSession::IsTargetForeground() const {
  if (!window_ || !IsWindow(window_)) return false;
  const HWND foreground = GetForegroundWindow();
  return foreground == window_ || (foreground && IsChild(window_, foreground));
}

bool AutomationSession::SendKeyInputs(const std::vector<INPUT>& inputs) {
  if (inputs.empty()) return true;
  if (!IsTargetForeground()) {
    focus_lost_ = true;
    return false;
  }
  return SendInput(static_cast<UINT>(inputs.size()), const_cast<INPUT*>(inputs.data()),
                   sizeof(INPUT)) == inputs.size();
}

bool AutomationSession::SendVirtualKey(WORD virtual_key) {
  INPUT down{};
  down.type = INPUT_KEYBOARD;
  down.ki.wVk = virtual_key;
  INPUT up = down;
  up.ki.dwFlags = KEYEVENTF_KEYUP;
  const bool sent = SendKeyInputs({down, up});
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  return sent;
}

bool AutomationSession::SendAscii(const std::string& text) {
  if (!FocusEditor()) return false;
  for (const unsigned char ch : text) {
    WORD key = 0;
    if (ch >= 'a' && ch <= 'z') {
      key = static_cast<WORD>('A' + (ch - 'a'));
    } else if (ch >= 'A' && ch <= 'Z') {
      key = static_cast<WORD>(ch);
    } else {
      return false;
    }
    if (!SendVirtualKey(key)) return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  return true;
}

bool AutomationSession::SendUnicode(std::wstring_view text) {
  if (!FocusEditor()) return false;
  std::vector<INPUT> inputs;
  inputs.reserve(text.size() * 2);
  for (const wchar_t code_unit : text) {
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wScan = code_unit;
    down.ki.dwFlags = KEYEVENTF_UNICODE;
    INPUT up = down;
    up.ki.dwFlags |= KEYEVENTF_KEYUP;
    inputs.push_back(down);
    inputs.push_back(up);
  }
  if (!SendKeyInputs(inputs)) return false;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  return true;
}

bool AutomationSession::SendModifiedKey(std::initializer_list<WORD> modifiers, WORD virtual_key) {
  std::vector<INPUT> inputs;
  inputs.reserve(modifiers.size() * 2 + 2);
  for (const WORD modifier : modifiers) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = modifier;
    inputs.push_back(input);
  }
  INPUT key_down{};
  key_down.type = INPUT_KEYBOARD;
  key_down.ki.wVk = virtual_key;
  inputs.push_back(key_down);
  INPUT key_up = key_down;
  key_up.ki.dwFlags = KEYEVENTF_KEYUP;
  inputs.push_back(key_up);
  for (auto it = modifiers.end(); it != modifiers.begin();) {
    --it;
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = *it;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    inputs.push_back(input);
  }
  const bool sent = SendKeyInputs(inputs);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  return sent;
}

bool AutomationSession::ClearEditor() {
  if (!FocusEditor()) return false;
  if (!SendVirtualKey(VK_ESCAPE)) return false;
  IUnknown* unknown = nullptr;
  if (SUCCEEDED(editor_->GetCurrentPattern(UIA_ValuePatternId, &unknown)) && unknown) {
    IUIAutomationValuePattern* value_pattern = nullptr;
    if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&value_pattern))) && value_pattern) {
      BSTR empty = SysAllocString(L"");
      const HRESULT hr = empty ? value_pattern->SetValue(empty) : E_OUTOFMEMORY;
      if (empty) SysFreeString(empty);
      SafeRelease(value_pattern);
      SafeRelease(unknown);
      if (SUCCEEDED(hr)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        return true;
      }
    }
    SafeRelease(unknown);
  }
  INPUT control_down{};
  control_down.type = INPUT_KEYBOARD;
  control_down.ki.wVk = VK_CONTROL;
  INPUT a_down{};
  a_down.type = INPUT_KEYBOARD;
  a_down.ki.wVk = 'A';
  INPUT a_up = a_down;
  a_up.ki.dwFlags = KEYEVENTF_KEYUP;
  INPUT control_up = control_down;
  control_up.ki.dwFlags = KEYEVENTF_KEYUP;
  if (!SendKeyInputs({control_down, a_down, a_up, control_up})) return false;
  return SendVirtualKey(VK_DELETE);
}

std::optional<std::wstring> AutomationSession::ReadEditorText() {
  if (!editor_) return std::nullopt;
  return ReadElementText(editor_);
}

std::optional<RECT> AutomationSession::CaretRect() const {
  if (!window_) return std::nullopt;
  const DWORD thread_id = GetWindowThreadProcessId(window_, nullptr);
  GUITHREADINFO info{};
  info.cbSize = sizeof(info);
  if (!GetGUIThreadInfo(thread_id, &info) || !info.hwndCaret) return std::nullopt;
  RECT rect = info.rcCaret;
  MapWindowPoints(info.hwndCaret, nullptr, reinterpret_cast<POINT*>(&rect), 2);
  return rect;
}

std::optional<RECT> AutomationSession::CandidateRect() const {
  if (!window_) return std::nullopt;
  DWORD target_process_id = 0;
  GetWindowThreadProcessId(window_, &target_process_id);
  struct Data {
    const std::wstring* class_name;
    DWORD process_id;
    HWND result{nullptr};
  } data{&target_.candidate_window_class, target_process_id};
  EnumWindows(
      [](HWND window, LPARAM param) -> BOOL {
        auto* data = reinterpret_cast<Data*>(param);
        DWORD process_id = 0;
        GetWindowThreadProcessId(window, &process_id);
        if (process_id != data->process_id || !IsWindowVisible(window)) return TRUE;
        wchar_t class_name[256]{};
        GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
        if (*data->class_name == class_name) {
          data->result = window;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&data));
  if (!data.result) return std::nullopt;
  RECT rect{};
  return GetWindowRect(data.result, &rect) ? std::optional<RECT>(rect) : std::nullopt;
}

bool AutomationSession::CaptureFailureArtifacts(
    const CaseResult& result, const std::filesystem::path& output_directory) const {
  if (result.failure_directory.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(result.failure_directory, ec);
  if (ec) return false;

  std::ofstream log(result.failure_directory / "failure.log", std::ios::binary | std::ios::trunc);
  log << "case=" << result.id << "\n"
      << "status=" << ResultStatusName(result.status) << "\n"
      << "reason_code=" << result.reason_code << "\n"
      << "editor_discovery_duration_ms=" << editor_discovery_duration_ms_ << "\n";
  if (!log.good()) return false;
  if (!window_) return true;

  std::vector<RECT> highlight_rects;
  RECT target_rect{};
  if (GetWindowRect(window_, &target_rect)) {
    highlight_rects.push_back(target_rect);
    log << "target_rect=" << target_rect.left << "," << target_rect.top << "," << target_rect.right
        << "," << target_rect.bottom << "\n";
  }
  if (const auto candidate = CandidateRect()) {
    highlight_rects.push_back(*candidate);
    log << "candidate_rect=" << candidate->left << "," << candidate->top << "," << candidate->right
        << "," << candidate->bottom << "\n";
  }
  log.flush();
  const auto screenshot = result.failure_directory / "screenshot.png";
  const bool captured = WriteGeometryOverlayPng(screenshot, highlight_rects);
  if (captured) {
    std::error_code copy_ec;
    std::filesystem::copy_file(
        screenshot,
        output_directory / "screenshots" /
            (target_.id + "_" + result.id + "_" + ResultStatusName(result.status) + ".png"),
        std::filesystem::copy_options::overwrite_existing, copy_ec);
  }
  return captured;
}

void AutomationSession::CloseLaunchedWindow() {
  const auto close_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(target_.close_grace_period_ms);
  const bool has_owned_surface =
      window_ && IsWindow(window_) && (owns_window_ || owns_document_tab_);
  const bool reused_document_is_selected =
      owns_document_tab_ && has_owned_surface &&
      TemporaryDocumentIsSelected(automation_, window_, temporary_document_);
  const bool may_cleanup_surface = owns_window_ || reused_document_is_selected;
  bool temporary_document_saved = true;
  if (has_owned_surface && may_cleanup_surface) {
    if (!temporary_document_.empty() && target_.save_temporary_document_before_close && editor_) {
      temporary_document_saved = ClearEditor() && SendModifiedKey({VK_CONTROL}, 'S');
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    if (owns_window_) {
      PostMessageW(window_, WM_CLOSE, 0, 0);
      while (IsWindow(window_) && std::chrono::steady_clock::now() < close_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    } else if (temporary_document_saved &&
               TemporaryDocumentIsSelected(automation_, window_, temporary_document_) &&
               FocusEditor()) {
      SendModifiedKey({VK_CONTROL}, 'W');
      while (IsWindow(window_) &&
             TemporaryDocumentIsSelected(automation_, window_, temporary_document_) &&
             std::chrono::steady_clock::now() < close_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  }
  if (owns_document_tab_ && has_owned_surface &&
      (!may_cleanup_surface || !temporary_document_saved)) {
    std::cerr << "Skipped temporary document cleanup because ownership or save confirmation was "
                 "unavailable\n";
  }
  if (owns_launched_process_ && process_info_.hProcess) {
    const auto now = std::chrono::steady_clock::now();
    const DWORD remaining_ms =
        now < close_deadline
            ? static_cast<DWORD>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(close_deadline - now)
                      .count())
            : 0;
    if (WaitForSingleObject(process_info_.hProcess, remaining_ms) == WAIT_TIMEOUT) {
      TerminateProcess(process_info_.hProcess, 0);
      WaitForSingleObject(process_info_.hProcess, 1000);
    }
  }
  window_ = nullptr;
  owns_window_ = false;
  owns_document_tab_ = false;
  owns_launched_process_ = false;
}

}  // namespace azookey::compat_test

namespace {

void PrintUsage() { std::cout << "compat_test [--target <target.json>] [--output <directory>]\n"; }

}  // namespace

int wmain(int argc, wchar_t** argv) {
  using namespace azookey::compat_test;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    std::cerr << "Per-monitor v2 DPI awareness is required\n";
    return 70;
  }
  std::filesystem::path target_path = AZOOKEY_COMPAT_DEFAULT_TARGET;
  std::filesystem::path output_path;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view argument = argv[i];
    if (argument == L"--help" || argument == L"-h") {
      PrintUsage();
      return 0;
    }
    if ((argument == L"--target" || argument == L"--output") && i + 1 >= argc) {
      PrintUsage();
      return 64;
    }
    if (argument == L"--target") {
      target_path = argv[++i];
    } else if (argument == L"--output") {
      output_path = argv[++i];
    } else {
      PrintUsage();
      return 64;
    }
  }

  const auto target = LoadTargetConfig(target_path);
  if (!target) {
    std::cerr << "Invalid target configuration\n";
    return 64;
  }
  if (output_path.empty()) output_path = CreateTimestampedOutputDirectory();
  if (!PrepareOutputDirectory(output_path)) {
    std::cerr << "Unable to create output directory\n";
    return 70;
  }

  const HRESULT com_result = OleInitialize(nullptr);
  if (FAILED(com_result)) {
    std::cerr << "COM initialization failed\n";
    return 70;
  }

  std::map<std::string, CaseDefinition> registered;
  for (const auto& definition : {
           MakeC001BasicInputCase(),
           MakeC002BackspaceCase(),
           MakeC003EscapeCase(),
           MakeC004CandidatePositionCase(),
           MakeC005MonitorClampCase(),
           MakeC006DpiScalingCase(),
           MakeC007SurrogatePairCase(),
           MakeC008UndoRedoCase(),
           MakeC009FocusTransitionCase(),
           MakeC010HostRecoveryCase(),
           MakeC011ShortcutRoutingCase(),
           MakeC012RomanizationCase(),
       }) {
    registered.emplace(definition.id, definition);
  }

  std::vector<CaseResult> results;
  {
    AutomationSession session(*target);
    std::string start_reason;
    const bool started = session.Start(&start_reason);
    for (const auto& case_id : target->cases) {
      CaseResult result;
      result.id = case_id;
      const auto started_at = std::chrono::steady_clock::now();
      if (!started) {
        result.status = ResultStatus::FailingSkip;
        result.reason_code = start_reason;
      } else if (const auto it = registered.find(case_id); it != registered.end()) {
        result = it->second.run(session);
      } else {
        result.status = ResultStatus::FailingSkip;
        result.reason_code = "case-not-registered";
      }
      result.duration_ms =
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started_at)
                                    .count());
      if (result.status != ResultStatus::Pass) {
        result.failure_directory = FailureDirectory(output_path, result);
        session.CaptureFailureArtifacts(result, output_path);
      }
      results.push_back(std::move(result));
    }
  }

  const bool wrote_report = WriteReports(output_path, *target, results);
  const auto summary = SummarizeResults(results);
  OleUninitialize();
  if (!wrote_report) return 70;
  std::cout << "Report: " << (output_path / "report.md").string() << "\n";
  if (summary.failed > 0) return 1;
  if (summary.failing_skipped > 0) return 2;
  return 0;
}
