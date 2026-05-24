#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace azookey::learning {

inline bool WriteTextFileAtomically(const std::string& path, const std::string& content) {
  const std::filesystem::path target(path);
  std::error_code ec;
  if (!target.parent_path().empty()) {
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) return false;
  }

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto temp = target;
  temp += ".tmp." + std::to_string(stamp);
  {
    std::ofstream ofs(temp, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
      return false;
    }
    ofs << content;
    ofs.flush();
    if (!ofs.good()) {
      std::filesystem::remove(temp, ec);
      return false;
    }
  }

#ifdef _WIN32
  const bool moved = MoveFileExW(temp.wstring().c_str(), target.wstring().c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  if (!moved) {
    std::filesystem::remove(temp, ec);
  }
  return moved;
#else
  std::filesystem::rename(temp, target, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    return false;
  }
  return true;
#endif
}

}  // namespace azookey::learning
