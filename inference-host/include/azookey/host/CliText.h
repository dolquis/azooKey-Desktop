#pragma once

#include <string>

namespace azookey::host {

inline std::string SanitizeTsvCell(std::string value) {
  for (char& ch : value) {
    if (ch == '\t' || ch == '\r' || ch == '\n') {
      ch = ' ';
    }
  }
  return value;
}

}  // namespace azookey::host
