#include "azookey/host/HostArgs.h"

#include <cstddef>
#include <utility>

namespace azookey::host {
namespace {

bool TakeValue(const std::vector<std::string>& argv, size_t& index, const std::string& option,
               std::string& value, std::optional<std::string>& error) {
  if (index + 1 >= argv.size()) {
    error = "missing value for " + option;
    return false;
  }
  value = argv[++index];
  return true;
}

}  // namespace

HostArgsParseResult ParseHostArgs(const std::vector<std::string>& argv, EngineConfig initial_config,
                                  std::string default_handshake_token) {
  HostArgsParseResult result;
  auto& args = result.args;
  args.config = std::move(initial_config);
  args.handshake_token = std::move(default_handshake_token);

  for (size_t i = 0; i < argv.size(); ++i) {
    const auto& arg = argv[i];
    if (arg == "userdict") {
      args.userdict_args.emplace(argv.begin() + static_cast<std::ptrdiff_t>(i + 1), argv.end());
      break;
    }
    if (arg == "lookup") {
      args.lookup_args.emplace(argv.begin() + static_cast<std::ptrdiff_t>(i + 1), argv.end());
      break;
    }
    if (arg == "--cuda") {
      args.config.backend = BackendKind::Cuda;
      args.explicit_backend = true;
      continue;
    }
    if (arg == "--cpu") {
      args.config.backend = BackendKind::Cpu;
      args.explicit_backend = true;
      continue;
    }
    if (arg == "--backend") {
      std::string value;
      if (!TakeValue(argv, i, arg, value, result.error)) return result;
      if (value == "cuda") {
        args.config.backend = BackendKind::Cuda;
      } else if (value == "cpu") {
        args.config.backend = BackendKind::Cpu;
      } else {
        result.error = "unsupported backend: " + value;
        return result;
      }
      args.explicit_backend = true;
      continue;
    }
    if (arg == "--model") {
      if (!TakeValue(argv, i, arg, args.config.model_path, result.error)) return result;
      args.explicit_model_path = true;
      continue;
    }
    if (arg == "--learning") {
      std::string value;
      if (!TakeValue(argv, i, arg, value, result.error)) return result;
      args.explicit_learning_path = std::move(value);
      continue;
    }
    if (arg == "--user-dict") {
      std::string value;
      if (!TakeValue(argv, i, arg, value, result.error)) return result;
      args.explicit_user_dict_path = std::move(value);
      continue;
    }
    if (arg == "--mock-dict") {
      if (!TakeValue(argv, i, arg, args.mock_dict_path, result.error)) return result;
      continue;
    }
    if (arg == "--pipe") {
      args.pipe_mode = true;
      if (i + 1 < argv.size() && argv[i + 1].rfind("--", 0) != 0) {
        args.pipe_name = argv[++i];
      }
      continue;
    }
    if (arg == "--pipe-name") {
      if (!TakeValue(argv, i, arg, args.pipe_name, result.error)) return result;
      args.pipe_mode = true;
      continue;
    }
    if (arg == "--handshake-token") {
      if (!TakeValue(argv, i, arg, args.handshake_token, result.error)) return result;
      continue;
    }
    if (arg == "--stdio") {
      args.pipe_mode = false;
      continue;
    }

    result.error = "unknown argument: " + arg;
    return result;
  }

  return result;
}

}  // namespace azookey::host
