#include "runner/CompatTypes.h"

#include <Ole2.h>
#include <UIAutomation.h>

#ifdef GetObject
#undef GetObject
#endif

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include "azookey/ipc/Json.h"
#include "runner/ReportWriter.h"
#include "runner/ScreenshotCapture.h"

namespace azookey::compat_test {
namespace {

template <typename T>
void SafeRelease(T*& value) {
  if (value) {
    value->Release();
    value = nullptr;
  }
}

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                          nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size);
  return result;
}

std::optional<TargetConfig> LoadTargetConfig(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::nullopt;
  std::ostringstream contents;
  contents << stream.rdbuf();
  const auto parsed = azookey::ipc::json::Parse(contents.str());
  if (!parsed || !parsed->IsObject()) return std::nullopt;

  TargetConfig config;
  const auto id = parsed->GetString("id");
  const auto display_name = parsed->GetString("display_name");
  const auto app_id = parsed->GetString("app_id");
  const auto automation_level = parsed->GetString("automation_level");
  const auto* launch = parsed->GetObject("launch");
  const auto* window = parsed->GetObject("window");
  const auto* cases = parsed->GetArray("cases");
  if (!id || !display_name || !app_id || !automation_level || !launch || !window || !cases) {
    return std::nullopt;
  }
  config.id = *id;
  config.display_name = *display_name;
  config.app_id = *app_id;
  config.automation_level = *automation_level;

  const auto executable = azookey::ipc::json::Value(*launch).GetString("executable");
  const auto window_class = azookey::ipc::json::Value(*window).GetString("class");
  const auto edit_class = azookey::ipc::json::Value(*window).GetString("edit_control_class");
  const auto candidate_class =
      azookey::ipc::json::Value(*window).GetString("candidate_window_class");
  if (!executable || !window_class || !edit_class || !candidate_class) return std::nullopt;
  config.executable = Utf8ToWide(*executable);
  config.window_classes.push_back(Utf8ToWide(*window_class));
  config.edit_control_class = Utf8ToWide(*edit_class);
  config.candidate_window_class = Utf8ToWide(*candidate_class);

  if (const auto* args = azookey::ipc::json::Value(*launch).GetArray("args")) {
    for (const auto& arg : *args) {
      if (!arg.IsString()) return std::nullopt;
      config.arguments.push_back(Utf8ToWide(arg.AsString()));
    }
  }
  if (const auto* fallbacks = azookey::ipc::json::Value(*window).GetArray("class_fallbacks")) {
    for (const auto& item : *fallbacks) {
      if (!item.IsString()) return std::nullopt;
      config.window_classes.push_back(Utf8ToWide(item.AsString()));
    }
  }
  for (const auto& item : *cases) {
    if (!item.IsString()) return std::nullopt;
    config.cases.push_back(item.AsString());
  }
  if (const auto* workarounds = parsed->GetObject("workarounds")) {
    config.use_temporary_document =
        azookey::ipc::json::Value(*workarounds).GetBool("use_temporary_document").value_or(false);
  }
  if (config.executable.empty() || config.window_classes.front().empty() ||
      config.edit_control_class.empty() || config.candidate_window_class.empty() ||
      config.cases.empty()) {
    return std::nullopt;
  }
  return config;
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

HWND FindNewTargetWindow(const TargetConfig& target, const std::set<HWND>& existing,
                         DWORD preferred_process_id) {
  struct Data {
    const TargetConfig* target;
    const std::set<HWND>* existing;
    DWORD preferred_process_id;
    HWND preferred{nullptr};
    HWND fallback{nullptr};
  } data{&target, &existing, preferred_process_id};
  EnumWindows(
      [](HWND window, LPARAM param) -> BOOL {
        auto* data = reinterpret_cast<Data*>(param);
        if (!IsConfiguredWindow(window, *data->target) || data->existing->contains(window)) {
          return TRUE;
        }
        DWORD process_id = 0;
        GetWindowThreadProcessId(window, &process_id);
        if (process_id == data->preferred_process_id) {
          data->preferred = window;
          return FALSE;
        }
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
        wchar_t image_path[32768]{};
        DWORD image_path_size = static_cast<DWORD>(std::size(image_path));
        const bool image_matches =
            process && QueryFullProcessImageNameW(process, 0, image_path, &image_path_size) &&
            _wcsicmp(std::filesystem::path(image_path).filename().c_str(),
                     std::filesystem::path(data->target->executable).filename().c_str()) == 0;
        if (process) CloseHandle(process);
        if (image_matches && !data->fallback) data->fallback = window;
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&data));
  return data.preferred ? data.preferred : data.fallback;
}

std::filesystem::path ResolveExecutable(const std::filesystem::path& configured) {
  if (configured.has_parent_path()) return configured;
  if (_wcsicmp(configured.c_str(), L"notepad.exe") == 0) {
    wchar_t system_directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system_directory, static_cast<UINT>(std::size(system_directory)));
    if (length > 0 && length < std::size(system_directory)) {
      return std::filesystem::path(system_directory) / configured;
    }
  }
  return configured;
}

std::filesystem::path CreateTemporaryDocument() {
  wchar_t temp_directory[MAX_PATH]{};
  const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temp_directory)), temp_directory);
  if (length == 0 || length >= std::size(temp_directory)) return {};
  for (uint32_t attempt = 0; attempt < 100; ++attempt) {
    const auto path = std::filesystem::path(temp_directory) /
                      (L"azookey-compat-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt) + L".txt");
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
      return path;
    }
  }
  return {};
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
      std::wstring result = SUCCEEDED(hr) && value ? std::wstring(value, SysStringLen(value))
                                                   : std::wstring{};
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
      std::wstring result = SUCCEEDED(hr) && value ? std::wstring(value, SysStringLen(value))
                                                   : std::wstring{};
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

std::filesystem::path FailureDirectory(const std::filesystem::path& output, const CaseResult& result) {
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
  std::wstring command = QuoteArgument(executable.wstring());
  for (const auto& argument : target_.arguments) command += L" " + QuoteArgument(argument);
  if (target_.use_temporary_document) {
    temporary_document_ = CreateTemporaryDocument();
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
  if (!CreateProcessW(nullptr, writable.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                      &process_info_)) {
    if (reason_code) *reason_code = "target-launch-failed";
    return false;
  }
  WaitForInputIdle(process_info_.hProcess, 5000);
  for (int attempt = 0; attempt < 100 && !window_; ++attempt) {
    window_ = FindNewTargetWindow(target_, existing, process_info_.dwProcessId);
    if (!window_) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!window_) {
    if (reason_code) *reason_code = "new-target-window-not-found";
    return false;
  }
  owns_window_ = true;
  if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&automation_)))) {
    if (reason_code) *reason_code = "uia-initialization-failed";
    return false;
  }
  if (!FindEditorElement()) {
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
  for (const CONTROLTYPEID control_type : {UIA_DocumentControlTypeId, UIA_EditControlTypeId}) {
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_I4;
    value.lVal = control_type;
    IUIAutomationCondition* condition = nullptr;
    if (SUCCEEDED(automation_->CreatePropertyCondition(UIA_ControlTypePropertyId, value,
                                                       &condition)) &&
        condition) {
      root->FindFirst(TreeScope_Descendants, condition, &editor_);
      SafeRelease(condition);
    }
    if (editor_) break;
  }
  if (!editor_ && !target_.edit_control_class.empty()) {
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(target_.edit_control_class.c_str());
    IUIAutomationCondition* condition = nullptr;
    if (value.bstrVal &&
        SUCCEEDED(automation_->CreatePropertyCondition(UIA_ClassNamePropertyId, value, &condition)) &&
        condition) {
      root->FindFirst(TreeScope_Descendants, condition, &editor_);
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
  for (const wchar_t code_unit : text) {
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wScan = code_unit;
    down.ki.dwFlags = KEYEVENTF_UNICODE;
    INPUT up = down;
    up.ki.dwFlags |= KEYEVENTF_KEYUP;
    if (!SendKeyInputs({down, up})) return false;
  }
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
      << "reason_code=" << result.reason_code << "\n";
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
        screenshot, output_directory / "screenshots" /
                        (target_.id + "_" + result.id + "_" + ResultStatusName(result.status) + ".png"),
        std::filesystem::copy_options::overwrite_existing, copy_ec);
  }
  return captured;
}

void AutomationSession::CloseLaunchedWindow() {
  if (owns_window_ && window_ && IsWindow(window_)) {
    if (!temporary_document_.empty() && editor_) {
      ClearEditor();
      INPUT control_down{};
      control_down.type = INPUT_KEYBOARD;
      control_down.ki.wVk = VK_CONTROL;
      INPUT s_down{};
      s_down.type = INPUT_KEYBOARD;
      s_down.ki.wVk = 'S';
      INPUT s_up = s_down;
      s_up.ki.dwFlags = KEYEVENTF_KEYUP;
      INPUT control_up = control_down;
      control_up.ki.dwFlags = KEYEVENTF_KEYUP;
      SendKeyInputs({control_down, s_down, s_up, control_up});
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    PostMessageW(window_, WM_CLOSE, 0, 0);
    for (int attempt = 0; attempt < 20 && IsWindow(window_); ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  if (process_info_.hProcess &&
      WaitForSingleObject(process_info_.hProcess, 500) == WAIT_TIMEOUT) {
    TerminateProcess(process_info_.hProcess, 0);
    WaitForSingleObject(process_info_.hProcess, 1000);
  }
  window_ = nullptr;
  owns_window_ = false;
}

}  // namespace azookey::compat_test

namespace {

void PrintUsage() {
  std::cout << "compat_test [--target <target.json>] [--output <directory>]\n";
}

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
      result.duration_ms = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
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
