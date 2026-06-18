#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/Dispatcher.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/RequestScheduler.h"
#include "azookey/host/SettingsStore.h"
#include "azookey/host/UserDataPaths.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/UserDictionary.h"

namespace {

constexpr const char* kHostVersion = "0.1.0";
volatile std::sig_atomic_t g_stop_requested = 0;

void HandleSignal(int) { g_stop_requested = 1; }

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
    std::cerr << "info: migrated legacy user data file " << legacy << " -> " << target
              << std::endl;
  }
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

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--cuda") {
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
  if (!azookey::host::EnsureUserDataDirectories(*user_paths)) {
    std::cerr << "error: failed to create azooKey user data directories under "
              << user_paths->root_dir << std::endl;
    return 2;
  }

  const std::string learning_path = user_paths->learning_path.string();
  const std::string user_dict_path = user_paths->user_dict_path.string();

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
    std::cerr << "warn: invalid settings.json";
    if (settings_result.error) std::cerr << ": " << *settings_result.error;
    if (settings_result.quarantined_path) {
      std::cerr << " (quarantined at " << *settings_result.quarantined_path << ")";
    }
    std::cerr << std::endl;
  }

  azookey::learning::LearningStore store(learning_path);
  store.Load();

  azookey::learning::UserDictionary user_dict(user_dict_path);
  user_dict.Load();

  auto converter = std::make_unique<azookey::core::SimpleConverter>();
  if (!mock_dict_path.empty()) {
    converter->LoadFromTsv(mock_dict_path);
  }

  azookey::host::InferenceEngine engine(std::move(converter), &store, config);
  engine.SetUserDictionary(&user_dict);
  if ((settings_store.settings().model.auto_load_on_host_start || explicit_model_path) &&
      !engine.LoadModel()) {
    std::cerr << "warn: model load failed: " << engine.last_error().value_or("unknown error")
              << " (falling back to SimpleConverter)" << std::endl;
  }

  azookey::host::RequestScheduler scheduler;
  azookey::host::DispatcherConfig dconf;
  dconf.host_version = kHostVersion;
  dconf.protocol_version = 1;
  dconf.default_backend = default_backend;
  if (explicit_backend) {
    dconf.override_backend = cli_backend;
  }
  if (explicit_model_path) {
    dconf.override_model_path = cli_model_path;
  }
  if (pipe_mode) {
    if (handshake_token.empty()) {
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

  std::cerr << "azookey inference-host started. backend="
            << (engine.backend() == azookey::host::BackendKind::Cuda ? "cuda" : "cpu")
            << " learning=" << learning_path << " user_dict=" << user_dict_path
            << " model_loaded=" << (engine.model_loaded() ? "true" : "false") << std::endl;

  if (pipe_mode) {
    if (pipe_name.empty()) {
      pipe_name = azookey::ipc::DefaultPipeName();
    }

    azookey::ipc::NamedPipeServer server;
    if (!server.Start(pipe_name,
                      [&engine, &scheduler, &user_dict, &settings_store, dconf]() {
                        auto d = std::make_shared<azookey::host::Dispatcher>(
                            &engine, &scheduler, &user_dict, dconf, &settings_store);
                        return [d](const azookey::ipc::Envelope& env) {
                          return d->Dispatch(env);
                        };
                      })) {
      std::cerr << "error: failed to start named pipe server: " << pipe_name << std::endl;
      return 2;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    std::cerr << "named pipe listening: " << pipe_name << std::endl;
    while (!g_stop_requested) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    server.Stop();
    return 0;
  }

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    auto env = azookey::ipc::Deserialize(line);
    if (!env) {
      std::cerr << "warn: failed to parse envelope" << std::endl;
      continue;
    }
    auto resp = stdio_dispatcher.Dispatch(*env);
    if (resp) {
      std::cout << azookey::ipc::Serialize(*resp) << std::endl;
      std::cout.flush();
    }
  }
  return 0;
}
