#pragma once

#include <filesystem>
#include <optional>

namespace azookey::core {

std::optional<std::filesystem::path> GetLocalAppDataDirectory() noexcept;

}  // namespace azookey::core
