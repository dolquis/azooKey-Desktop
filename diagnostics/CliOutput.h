#pragma once

#include <filesystem>
#include <string>
#include <utility>

#include "Diagnostics.h"
#include "azookey/core/PlatformPaths.h"
#include "azookey/ipc/Json.h"

namespace azookey::diagnostics::detail {

inline std::string SerializeCollectionResult(const std::filesystem::path& output, Status status) {
  ipc::json::Object response;
  response.emplace("output", ipc::json::Value(core::PathToUtf8(output)));
  response.emplace("status", ipc::json::Value(StatusName(status)));
  return ipc::json::Stringify(ipc::json::Value(std::move(response)));
}

}  // namespace azookey::diagnostics::detail
