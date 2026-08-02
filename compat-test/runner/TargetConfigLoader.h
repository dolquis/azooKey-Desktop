#pragma once

#include <filesystem>
#include <optional>

#include "runner/CompatTypes.h"

namespace azookey::compat_test {

enum class ExecutableResolutionSource {
  ConfiguredPath,
  SearchPath,
  AppPaths,
  Unresolved,
};

struct ExecutableResolution {
  std::filesystem::path path;
  ExecutableResolutionSource source{ExecutableResolutionSource::Unresolved};
};

bool IsValidTargetId(std::string_view id);
std::optional<TargetConfig> LoadTargetConfig(const std::filesystem::path& path);
ExecutableResolution ResolveExecutable(const std::filesystem::path& configured);
const char* ExecutableResolutionSourceName(ExecutableResolutionSource source);
std::filesystem::path CreateTemporaryDocument(const TargetConfig& target);

}  // namespace azookey::compat_test
