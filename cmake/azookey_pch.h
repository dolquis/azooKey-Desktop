// Precompiled header used by the DEV-910 PCH measurement
// (-DAZOOKEY_USE_PCH=ON). The contents are derived from the actual include
// graph of a Debug build: the standard-library headers below appear in at
// least half of the project's translation units, and <windows.h> in 58%.
//
// NOMINMAX / WIN32_LEAN_AND_MEAN mirror what the .cpp files already define
// before including <windows.h>, so the force-included PCH agrees with them and
// the repeated #define is a benign identical redefinition rather than C4005.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// wingdi.h defines GetObject as GetObjectW. Without the PCH, <windows.h> is
// included after the project headers, so ipc::json::Value::GetObject is
// declared before the macro exists and core/src/AppProfileResolver.cpp only
// has to undefine it around its own call sites. The PCH inverts that order:
// windows.h now comes first, so the macro would rename the declaration too.
// Undefining it here restores the original order's meaning.
#ifdef GetObject
#undef GetObject
#endif
#endif
