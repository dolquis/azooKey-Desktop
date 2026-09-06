#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace azookey::core {

std::optional<std::filesystem::path> GetLocalAppDataDirectory() noexcept;

// Build a path from UTF-8 bytes. The narrow `std::filesystem::path`
// constructor decodes with the platform active code page on Windows, which
// loses characters outside it; these two keep UTF-8 the single narrow encoding
// on both sides of the path boundary.
std::filesystem::path Utf8Path(std::string_view value);
std::string PathToUtf8(const std::filesystem::path& path);

}  // namespace azookey::core
