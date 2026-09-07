#pragma once

#include <filesystem>
#include <memory>
#include <mutex>

#include "azookey/core/RewriterIndex.h"
#include "azookey/logging/RuntimeLogger.h"

namespace azookey::host {
struct RewriterConfig {
  bool symbol_enabled{false};
  bool emoji_enabled{false};
  bool trigger_enabled{true};
  std::filesystem::path symbol_path;
  std::filesystem::path emoji_path;
};

class RewriterData {
 public:
  std::shared_ptr<const core::RewriterIndex> Get(bool emoji, const RewriterConfig& config,
                                                 logging::RuntimeLogger* logger);

 private:
  struct Slot {
    std::mutex mutex;
    bool attempted{false};
    std::filesystem::path path;
    std::shared_ptr<const core::RewriterIndex> index;
  };
  Slot symbol_;
  Slot emoji_;
};
}  // namespace azookey::host
