#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::bench {

std::filesystem::path Utf8Path(std::string_view value);
std::vector<std::string> Utf8CommandLineArguments(int argc, char** argv);

}  // namespace azookey::bench
