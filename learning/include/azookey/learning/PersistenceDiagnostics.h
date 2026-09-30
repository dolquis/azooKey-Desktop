#pragma once

#include <iostream>
#include <syncstream>
#include <system_error>

namespace azookey::learning::detail {

// Only fixed operation names and numeric OS errors belong on stderr. Never
// include a path, file contents, or the localized error message.
inline bool ReportPersistenceFailure(const char* stage, std::error_code error = {}) {
  std::osyncstream(std::cerr) << "persistence error: stage=" << stage
                              << " category=" << error.category().name()
                              << " code=" << error.value() << '\n';
  return false;
}

}  // namespace azookey::learning::detail
