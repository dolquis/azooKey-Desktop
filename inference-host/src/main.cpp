#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <signal.h>
#endif

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/Dispatcher.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/RequestScheduler.h"
#include "azookey/host/SettingsStore.h"
#include "azookey/host/UserDataPaths.h"
#include "azookey/host/UserDictCli.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/UserDictionary.h"
#include "azookey/logging/RuntimeLogger.h"

namespace {

azookey::logging::RuntimeLogSafeText SafeLogText(std::string value) {
  return azookey::logging::RuntimeLogSafeText(std::move(value));
}

constexpr const char* kHostVersion = "0.1.0";
volatile std::sig_atomic_t g_signal_stop_requested = 0;

std::string CreateHostGenerationId() {
  std::array<unsigned char, 16> bytes{};
  std::random_device random;
  for (auto& byte : bytes) {
    byte = static_cast<unsigned char>(random());
  }
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);

  char buffer[37]{};
  const auto as_uint = [](unsigned char value) { return static_cast<unsigned int>(value); };
  const int written =
      std::snprintf(buffer, sizeof(buffer),
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    as_uint(bytes[0]), as_uint(bytes[1]), as_uint(bytes[2]), as_uint(bytes[3]),
                    as_uint(bytes[4]), as_uint(bytes[5]), as_uint(bytes[6]), as_uint(bytes[7]),
                    as_uint(bytes[8]), as_uint(bytes[9]), as_uint(bytes[10]), as_uint(bytes[11]),
                    as_uint(bytes[12]), as_uint(bytes[13]), as_uint(bytes[14]), as_uint(bytes[15]));
  return written == 36 ? std::string(buffer, 36) : std::string();
}

void HandleSignal(int) { g_signal_stop_requested = 1; }

#ifdef _WIN32
std::atomic<bool> g_console_stop_requested{false};
HANDLE g_main_thread = nullptr;
HANDLE g_shutdown_complete = nullptr;

BOOL WINAPI HandleConsoleControl(DWORD control_type) {
  switch (control_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
      g_console_stop_requested.store(true, std::memory_order_relaxed);
      // StopRequested() may have just been checked before stdio enters a
      // blocking read. Retry cancellation while main unwinds to close that
      // race without performing persistence work in the control handler.
      for (DWORD waited_ms = 0; waited_ms < 4500; waited_ms += 10) {
        if (g_main_thread != nullptr) {
          CancelSynchronousIo(g_main_thread);
        }
        if (g_shutdown_complete != nullptr &&
            WaitForSingleObject(g_shutdown_complete, 10) == WAIT_OBJECT_0) {
          break;
        }
      }
      return TRUE;
    default:
      return FALSE;
  }
}

class ConsoleControlRegistration {
 public:
  ConsoleControlRegistration() = default;
  ConsoleControlRegistration(const ConsoleControlRegistration&) = delete;
  ConsoleControlRegistration& operator=(const ConsoleControlRegistration&) = delete;

  bool Register() {
    g_shutdown_complete = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_shutdown_complete == nullptr) return false;

    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &g_main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      return false;
    }
    registered_ = SetConsoleCtrlHandler(HandleConsoleControl, TRUE) != FALSE;
    return registered_;
  }

  ~ConsoleControlRegistration() {
    if (g_shutdown_complete != nullptr) {
      SetEvent(g_shutdown_complete);
    }
    if (registered_) {
      SetConsoleCtrlHandler(HandleConsoleControl, FALSE);
    }
    // These process-lifetime handles intentionally remain valid until all
    // concurrently running console control handlers have returned.
  }

 private:
  bool registered_ = false;
};
#endif

bool StopRequested() {
  if (g_signal_stop_requested != 0) return true;
#ifdef _WIN32
  return g_console_stop_requested.load(std::memory_order_relaxed);
#else
  return false;
#endif
}

bool RegisterSignalHandlers() {
#ifdef _WIN32
  return std::signal(SIGINT, HandleSignal) != SIG_ERR &&
         std::signal(SIGTERM, HandleSignal) != SIG_ERR;
#else
  struct sigaction action {};
  action.sa_handler = HandleSignal;
  sigemptyset(&action.sa_mask);
  // Deliberately omit SA_RESTART so a blocked stdio read returns on shutdown.
  action.sa_flags = 0;
  return sigaction(SIGINT, &action, nullptr) == 0 && sigaction(SIGTERM, &action, nullptr) == 0;
#endif
}

void ApplyDefaultBackend(azookey::host::EngineConfig& config) {
  const std::string backend = AZOOKEY_BACKEND_DEFAULT;
  if (backend == "cuda") {
    config.backend = azookey::host::BackendKind::Cuda;
  } else {
    config.backend = azookey::host::BackendKind::Cpu;
  }
}

#ifdef _WIN32
std::filesystem::path GetExeDirectory() {
  wchar_t buf[MAX_PATH]{};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path();
}
#endif

void MigrateLegacyDefaultFileIfNeeded(const char* legacy_name,
                                      const std::filesystem::path& target) {
#ifdef _WIN32
  // Use the exe directory rather than CWD so the lookup is deterministic and
  // limited to installer-controlled paths.
  const std::filesystem::path legacy = GetExeDirectory() / legacy_name;
#else
  const std::filesystem::path legacy(legacy_name);
#endif
  if (std::filesystem::exists(target) || !std::filesystem::exists(legacy)) {
    return;
  }

  std::error_code ec;
  if (!target.parent_path().empty()) {
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) return;
  }
  std::filesystem::copy_file(legacy, target, std::filesystem::copy_options::none, ec);
  if (!ec) {
    std::cerr << "info: migrated legacy user data file " << legacy << " -> " << target << std::endl;
  }
}

bool EnsureFileParentDirectory(const std::filesystem::path& file_path) {
  const auto parent = file_path.parent_path();
  if (parent.empty()) {
    return true;
  }
  std::error_code ec;
  std::filesystem::create_directories(parent, ec);
  return !ec;
}

std::string GetEnvString(const char* name) {
#if defined(_MSC_VER)
  char* value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
    return {};
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
#endif
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // Construct before the engine so its destructor signals completion only
  // after the engine destructor has flushed pending learning observations.
  ConsoleControlRegistration console_control;
#endif
  azookey::host::EngineConfig config;
  ApplyDefaultBackend(config);
  const auto default_backend = config.backend;
  std::optional<std::filesystem::path> explicit_learning_path;
  std::optional<std::filesystem::path> explicit_user_dict_path;
  std::string mock_dict_path;
  bool explicit_backend = false;
  bool explicit_model_path = false;
  bool pipe_mode = false;
  std::string pipe_name;
  std::string handshake_token = GetEnvString("AZOOKEY_IPC_HANDSHAKE_TOKEN");
  std::optional<std::vector<std::string>> userdict_args;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "userdict") {
      userdict_args.emplace();
      for (int j = i + 1; j < argc; ++j) {
        userdict_args->push_back(argv[j]);
      }
      break;
    } else if (arg == "--cuda") {
      config.backend = azookey::host::BackendKind::Cuda;
      explicit_backend = true;
    } else if (arg == "--cpu") {
      config.backend = azookey::host::BackendKind::Cpu;
      explicit_backend = true;
    } else if (arg == "--backend" && i + 1 < argc) {
      const std::string value = argv[++i];
      if (value == "cuda") {
        config.backend = azookey::host::BackendKind::Cuda;
        explicit_backend = true;
      } else if (value == "cpu") {
        config.backend = azookey::host::BackendKind::Cpu;
        explicit_backend = true;
      }
    } else if (arg == "--model" && i + 1 < argc) {
      config.model_path = argv[++i];
      explicit_model_path = true;
    } else if (arg == "--learning" && i + 1 < argc) {
      explicit_learning_path = argv[++i];
    } else if (arg == "--user-dict" && i + 1 < argc) {
      explicit_user_dict_path = argv[++i];
    } else if (arg == "--mock-dict" && i + 1 < argc) {
      mock_dict_path = argv[++i];
    } else if (arg == "--pipe") {
      pipe_mode = true;
      if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        pipe_name = argv[++i];
      }
    } else if (arg == "--pipe-name" && i + 1 < argc) {
      pipe_mode = true;
      pipe_name = argv[++i];
    } else if (arg == "--handshake-token" && i + 1 < argc) {
      handshake_token = argv[++i];
    } else if (arg == "--stdio") {
      pipe_mode = false;
    }
  }

  azookey::host::UserDataPathInputs path_inputs;
  path_inputs.local_app_data = azookey::host::GetPlatformLocalAppData();
  path_inputs.explicit_learning_path = explicit_learning_path;
  path_inputs.explicit_user_dict_path = explicit_user_dict_path;
  auto user_paths = azookey::host::ResolveUserDataPaths(path_inputs);
  if (!user_paths) {
    std::cerr << "error: failed to resolve azooKey user data directory" << std::endl;
    return 2;
  }

  const std::string learning_path = user_paths->learning_path.string();
  const std::string user_dict_path = user_paths->user_dict_path.string();

  if (userdict_args) {
    if (!EnsureFileParentDirectory(user_paths->user_dict_path)) {
      std::cerr << "error: failed to create user dictionary directory: "
                << user_paths->user_dict_path.parent_path() << std::endl;
      return 2;
    }
    if (!explicit_user_dict_path) {
      MigrateLegacyDefaultFileIfNeeded("azookey_user_dict.json", user_paths->user_dict_path);
    }

    std::string parse_error;
    auto cli_options = azookey::host::ParseUserDictCliArgs(*userdict_args, &parse_error);
    if (!cli_options) {
      std::cerr << "error: " << parse_error << std::endl;
      return 2;
    }
    azookey::host::UserDictCliRunOptions run_options;
    run_options.user_dict_path = user_dict_path;
    run_options.pipe_name = pipe_name.empty() ? azookey::ipc::DefaultPipeName() : pipe_name;
    run_options.handshake_token = handshake_token;
    auto result = azookey::host::RunUserDictCli(*cli_options, run_options);
    for (const auto& line : result.output_lines) {
      std::cout << line << std::endl;
    }
    if (!result.error.empty()) {
      std::cerr << "error: " << result.error << std::endl;
    }
    return result.exit_code;
  }

  azookey::logging::RuntimeLogger runtime_log(
      azookey::logging::RuntimeLoggerOptionsFromEnvironment("host", user_paths->logs_dir));
  if (!azookey::host::EnsureUserDataDirectories(*user_paths)) {
    runtime_log.Log(azookey::logging::RuntimeLogLevel::Error, "user_data_directory_create_failed");
    std::cerr << "error: failed to create azooKey user data directories under "
              << user_paths->root_dir << std::endl;
    return 2;
  }

  if (!explicit_learning_path) {
    MigrateLegacyDefaultFileIfNeeded("azookey_learning.tsv", user_paths->learning_path);
  }
  if (!explicit_user_dict_path) {
    MigrateLegacyDefaultFileIfNeeded("azookey_user_dict.json", user_paths->user_dict_path);
  }

  azookey::host::SettingsStore settings_store(user_paths->settings_path);
  const auto settings_result = settings_store.Load();
  const auto cli_backend = config.backend;
  const auto cli_model_path = config.model_path;
  config = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, settings_result.settings,
                                                             default_backend);
  if (explicit_backend) {
    config.backend = cli_backend;
  }
  if (explicit_model_path) {
    config.model_path = cli_model_path;
  }
  if (settings_result.status == azookey::host::SettingsLoadStatus::Invalid) {
    runtime_log.Log(azookey::logging::RuntimeLogLevel::Warn, "settings_load_failed",
                    {{"result", SafeLogText("error")}});
    std::cerr << "warn: invalid settings.json";
    if (settings_result.error) std::cerr << ": " << *settings_result.error;
    if (settings_result.quarantined_path) {
      std::cerr << " (quarantined at " << *settings_result.quarantined_path << ")";
    }
    std::cerr << std::endl;
  }

  std::error_code learning_path_error;
  const bool learning_file_existed =
      std::filesystem::exists(user_paths->learning_path, learning_path_error) &&
      !learning_path_error;
  azookey::learning::LearningStore store(learning_path);
  const bool learning_loaded = store.Load();
  const bool learning_load_failed = learning_file_existed && !learning_loaded;
  runtime_log.Log(
      learning_load_failed ? azookey::logging::RuntimeLogLevel::Warn
                           : azookey::logging::RuntimeLogLevel::Info,
      "learning_load",
      {{"result",
        SafeLogText(learning_loaded ? "ok" : (learning_file_existed ? "error" : "missing"))}});

  azookey::learning::UserDictionary user_dict(user_dict_path);
  const bool user_dict_loaded = user_dict.Load();
  runtime_log.Log(user_dict_loaded ? azookey::logging::RuntimeLogLevel::Info
                                   : azookey::logging::RuntimeLogLevel::Warn,
                  "user_dictionary_load",
                  {{"result", SafeLogText(user_dict_loaded ? "ok" : "error")}});

  auto converter = std::make_unique<azookey::core::SimpleConverter>();
  if (!mock_dict_path.empty()) {
    converter->LoadFromTsv(mock_dict_path);
  }

  azookey::host::InferenceEngine engine(std::move(converter), &store, config);
  engine.SetUserDictionary(&user_dict);
  if (explicit_model_path) {
    const bool model_loaded = engine.LoadModel();
    runtime_log.Log(model_loaded ? azookey::logging::RuntimeLogLevel::Info
                                 : azookey::logging::RuntimeLogLevel::Error,
                    "model_load", {{"result", SafeLogText(model_loaded ? "ok" : "error")}});
    if (!model_loaded) {
      std::cerr << "warn: model load failed: " << engine.last_error().value_or("unknown error")
                << " (falling back to SimpleConverter)" << std::endl;
    }
  } else if (settings_store.settings().model.auto_load_on_host_start) {
    const bool preload_started = engine.StartModelPreload(
        azookey::host::ModelLoadOptions{config.model_path, config.backend, config.n_gpu_layers});
    runtime_log.Log(preload_started ? azookey::logging::RuntimeLogLevel::Info
                                    : azookey::logging::RuntimeLogLevel::Warn,
                    "model_preload_start",
                    {{"result", SafeLogText(preload_started ? "ok" : "error")}});
  }

  azookey::host::RequestScheduler scheduler;
  azookey::host::DispatcherConfig dconf;
  dconf.host_version = kHostVersion;
  dconf.protocol_version = 1;
  dconf.host_generation_id = CreateHostGenerationId();
  if (dconf.host_generation_id.empty()) {
    runtime_log.Log(azookey::logging::RuntimeLogLevel::Warn,
                    "host_generation_id_generation_failed");
    std::cerr << "warn: failed to generate the inference Host generation ID" << std::endl;
  }
#if AZOOKEY_WITH_LLAMA_CPP
  dconf.runtime_tier = "llama_cpp";
#else
  dconf.runtime_tier = "mock";
#endif
  dconf.default_backend = default_backend;
  if (explicit_backend) {
    dconf.override_backend = cli_backend;
  }
  if (explicit_model_path) {
    dconf.override_model_path = cli_model_path;
  }
  if (pipe_mode) {
    if (handshake_token.empty()) {
      runtime_log.Log(azookey::logging::RuntimeLogLevel::Warn, "ipc_handshake_token_missing");
      std::cerr << "warn: no IPC handshake token configured; relying on per-user pipe ACL"
                << std::endl;
    }
    dconf.handshake_token = handshake_token;
  }
  // For stdio mode a single Dispatcher suffices (one connection).
  // For pipe mode a new Dispatcher is created per client connection so that
  // each client's authentication state is isolated.
  azookey::host::Dispatcher stdio_dispatcher(&engine, &scheduler, &user_dict, dconf,
                                             &settings_store);

  const auto startup_health = engine.health_snapshot();
  std::cerr << "azookey inference-host started. backend="
            << (startup_health.backend == azookey::host::BackendKind::Cuda ? "cuda" : "cpu")
            << " learning=" << learning_path << " user_dict=" << user_dict_path
            << " model_loaded=" << (startup_health.model_loaded ? "true" : "false");
  if (startup_health.model_preload_in_progress) {
    std::cerr << " model_preload=preloading";
  }
  std::cerr << std::endl;
  runtime_log.Log(
      azookey::logging::RuntimeLogLevel::Info, "backend_selected",
      {{"backend",
        SafeLogText(startup_health.backend == azookey::host::BackendKind::Cuda ? "cuda" : "cpu")}});
  runtime_log.Log(
      azookey::logging::RuntimeLogLevel::Info, "host_started",
      {{"backend",
        SafeLogText(startup_health.backend == azookey::host::BackendKind::Cuda ? "cuda" : "cpu")},
       {"model_loaded", startup_health.model_loaded},
       {"model_preload_in_progress", startup_health.model_preload_in_progress}});

  if (!RegisterSignalHandlers()) {
    runtime_log.Log(azookey::logging::RuntimeLogLevel::Warn,
                    "shutdown_signal_handler_register_failed");
    std::cerr << "warn: failed to register process shutdown signal handlers" << std::endl;
  }
#ifdef _WIN32
  if (!console_control.Register()) {
    runtime_log.Log(azookey::logging::RuntimeLogLevel::Warn,
                    "console_shutdown_handler_register_failed");
    std::cerr << "warn: failed to register Windows console shutdown handler" << std::endl;
  }
#endif

  if (pipe_mode) {
    if (pipe_name.empty()) {
      pipe_name = azookey::ipc::DefaultPipeName();
    }

    azookey::ipc::NamedPipeServer server;
    if (!server.Start(pipe_name, [&engine, &scheduler, &user_dict, &settings_store, dconf]() {
          auto d = std::make_shared<azookey::host::Dispatcher>(&engine, &scheduler, &user_dict,
                                                               dconf, &settings_store);
          return [d](const azookey::ipc::Envelope& env) { return d->Dispatch(env); };
        })) {
      runtime_log.Log(azookey::logging::RuntimeLogLevel::Error, "pipe_listen_failed",
                      {{"result", SafeLogText("error")}});
      std::cerr << "error: failed to start named pipe server: " << pipe_name << std::endl;
      return 2;
    }

    runtime_log.Log(azookey::logging::RuntimeLogLevel::Info, "pipe_listening",
                    {{"result", SafeLogText("ok")}});
    std::cerr << "named pipe listening: " << pipe_name << std::endl;
    while (!StopRequested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    server.Stop();
    runtime_log.Log(azookey::logging::RuntimeLogLevel::Info, "host_stopped",
                    {{"result", SafeLogText("ok")}});
    return 0;
  }

  std::string line;
  while (!StopRequested() && std::getline(std::cin, line)) {
    if (line.empty()) continue;
    auto env = azookey::ipc::Deserialize(line);
    if (!env) {
      runtime_log.Log(azookey::logging::RuntimeLogLevel::Warn, "stdio_envelope_parse_failed",
                      {{"result", SafeLogText("error")}});
      std::cerr << "warn: failed to parse envelope" << std::endl;
      continue;
    }
    auto resp = stdio_dispatcher.Dispatch(*env);
    if (resp) {
      if (auto serialized = azookey::ipc::Serialize(*resp)) {
        std::cout << *serialized << std::endl;
        std::cout.flush();
      } else {
        runtime_log.Log(azookey::logging::RuntimeLogLevel::Warn, "stdio_envelope_serialize_failed",
                        {{"request_id", env->request_id}, {"result", SafeLogText("error")}});
        std::cerr << "warn: failed to serialize response envelope" << std::endl;
      }
    }
  }
  runtime_log.Log(azookey::logging::RuntimeLogLevel::Info, "host_stopped",
                  {{"result", SafeLogText("ok")}});
  return 0;
}
