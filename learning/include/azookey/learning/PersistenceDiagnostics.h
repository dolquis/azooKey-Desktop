#pragma once

#include <iostream>
#include <string>
#include <system_error>

namespace azookey::learning::detail {

// Only fixed operation names and numeric OS errors belong on stderr. Never
// include a path, file contents, or the localized error message.
//
// The line is built first and written with one insertion so reports from
// different threads do not interleave. std::osyncstream gives the same
// guarantee but imports msvcp140_atomic_wait.dll, which the MSI does not ship
// (DEV-1466).
inline bool ReportPersistenceFailure(const char* stage, std::error_code error = {}) {
  std::string line = "persistence error: stage=";
  line += stage;
  line += " category=";
  line += error.category().name();
  line += " code=";
  line += std::to_string(error.value());
  line += '\n';
  std::cerr << line;
  return false;
}

}  // namespace azookey::learning::detail
