#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef AZOOKEY_IPC_TESTDATA_DIR
#error "AZOOKEY_IPC_TESTDATA_DIR must point to ipc/testdata"
#endif

namespace azookey::ipc::test {

inline std::string ReadTextFixture(std::string_view filename) {
  const auto path = std::filesystem::path(AZOOKEY_IPC_TESTDATA_DIR) / filename;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open IPC test fixture: " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}  // namespace azookey::ipc::test
