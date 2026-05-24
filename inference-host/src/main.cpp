#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/Dispatcher.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/RequestScheduler.h"
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

std::string GenerateHandshakeToken() {
  std::random_device rd;
  std::ostringstream out;
  for (int i = 0; i < 32; ++i) {
    const auto byte = static_cast<unsigned>(rd() & 0xFFu);
    out << std::hex << std::setw(2) << std::setfill('0') << byte;
  }
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  azookey::host::EngineConfig config;
  ApplyDefaultBackend(config);
  std::optional<std::filesystem::path> explicit_learning_path;
  std::optional<std::filesystem::path> explicit_user_dict_path;
  std::string mock_dict_path;
  bool pipe_mode = false;
  std::string pipe_name;
  std::string handshake_token = GetEnvString("AZOOKEY_IPC_HANDSHAKE_TOKEN");

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--cuda") {
      config.backend = azookey::host::BackendKind::Cuda;
    } else if (arg == "--cpu") {
      config.backend = azookey::host::BackendKind::Cpu;
    } else if (arg == "--backend" && i + 1 < argc) {
      const std::string value = argv[++i];
      if (value == "cuda") {
        config.backend = azookey::host::BackendKind::Cuda;
      } else if (value == "cpu") {
        config.backend = azookey::host::BackendKind::Cpu;
      }
    } else if (arg == "--model" && i + 1 < argc) {
      config.model_path = argv[++i];
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
  if (!engine.LoadModel()) {
    std::cerr << "warn: model load failed: " << engine.last_error().value_or("unknown error")
              << " (falling back to SimpleConverter)" << std::endl;
  }

  azookey::host::RequestScheduler scheduler;
  azookey::host::DispatcherConfig dconf;
  dconf.host_version = kHostVersion;
  dconf.protocol_version = 1;
  if (pipe_mode) {
    if (handshake_token.empty()) {
      handshake_token = GenerateHandshakeToken();
      std::cerr << "warn: generated IPC handshake token for this host process; "
                   "manual TIP connections should set AZOOKEY_IPC_HANDSHAKE_TOKEN or "
                   "--handshake-token explicitly"
                << std::endl;
    }
    dconf.handshake_token = handshake_token;
  }
  azookey::host::Dispatcher dispatcher(&engine, &scheduler, &user_dict, dconf);

  std::cerr << "azookey inference-host started. backend="
            << (engine.backend() == azookey::host::BackendKind::Cuda ? "cuda" : "cpu")
            << " learning=" << learning_path << " user_dict=" << user_dict_path
            << " model_loaded=" << (engine.model_loaded() ? "true" : "false") << std::endl;

  if (pipe_mode) {
    if (pipe_name.empty()) {
      pipe_name = azookey::ipc::DefaultPipeName();
    }

    azookey::ipc::NamedPipeServer server;
    if (!server.Start(pipe_name, [&dispatcher](const azookey::ipc::Envelope& env) {
          return dispatcher.Dispatch(env);
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
    auto resp = dispatcher.Dispatch(*env);
    if (resp) {
      std::cout << azookey::ipc::Serialize(*resp) << std::endl;
      std::cout.flush();
    }
  }
  return 0;
}
