#include "azookey/host/RewriterData.h"

#include <fstream>
#include <iterator>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace azookey::host {
namespace {
std::filesystem::path DataPath(bool emoji) {
#ifdef _WIN32
  std::wstring buffer(32768, L'\0');
  const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (size == 0 || size == buffer.size()) return {};
  buffer.resize(size);
  const std::filesystem::path executable(buffer);
#else
  std::error_code ec;
  const auto executable = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) return {};
#endif
  return executable.parent_path() / "data" / (emoji ? "emoji.tsv" : "symbol.tsv");
}
}  // namespace

std::shared_ptr<const core::RewriterIndex> RewriterData::Get(bool emoji,
                                                             const RewriterConfig& config,
                                                             logging::RuntimeLogger* logger) {
  if (!(emoji ? config.emoji_enabled : config.symbol_enabled)) return {};
  auto& slot = emoji ? emoji_ : symbol_;
  std::lock_guard lock(slot.mutex);
  const auto configured = emoji ? config.emoji_path : config.symbol_path;
  const auto path = (configured.empty() ? DataPath(emoji) : configured).lexically_normal();
  if (slot.attempted && slot.path == path) return slot.index;
  slot.path = path;
  slot.attempted = true;
  slot.index.reset();
  std::ifstream stream(path, std::ios::binary);
  size_t invalid = 0;
  if (stream) {
    const std::string text((std::istreambuf_iterator<char>(stream)), {});
    if (!stream.bad()) {
      auto index = std::make_shared<core::RewriterIndex>(emoji ? core::CandidateSource::Emoji
                                                               : core::CandidateSource::Symbol);
      invalid = index->Parse(text);
      slot.index = std::move(index);
    }
  }
  if (!slot.index || invalid != 0) {
    // Do not log the configured path or data contents.
    const auto event = emoji ? "emoji-data-load-warning" : "symbol-data-load-warning";
    if (logger)
      logger->Log(logging::RuntimeLogLevel::Warn, event,
                  {{"loaded", static_cast<bool>(slot.index)},
                   {"invalid_rows", static_cast<uint64_t>(invalid)}});
  }
  return slot.index;
}
}  // namespace azookey::host
