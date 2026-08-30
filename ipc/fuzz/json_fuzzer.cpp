#include <cstddef>
#include <cstdint>
#include <string_view>

#include "azookey/ipc/Json.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto input = std::string_view(reinterpret_cast<const char*>(data), size);
  (void)azookey::ipc::json::Parse(input);
  return 0;
}
