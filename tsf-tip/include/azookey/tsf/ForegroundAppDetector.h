#pragma once

#include <Windows.h>

#include <optional>
#include <string_view>
#include <utility>

#include "azookey/core/BracketSettings.h"

namespace azookey::tsf {

// Owner-thread only. Never reads window titles or sends identity over IPC.
class ForegroundAppDetector final {
 public:
  core::ForegroundApp Get();
  void Invalidate() { cached_ = {}; }
#ifdef AZOOKEY_TSF_TESTING
  void SetForTest(core::ForegroundApp app) { test_app_ = std::move(app); }
#endif
 private:
  core::ForegroundApp cached_;
#ifdef AZOOKEY_TSF_TESTING
  std::optional<core::ForegroundApp> test_app_;
#endif
};

bool WindowsAppNameEqual(std::string_view left, std::string_view right);
}  // namespace azookey::tsf
