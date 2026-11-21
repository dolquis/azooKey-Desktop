#include "azookey/host/HomophoneNotesData.h"

#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace azookey::host {
namespace {
std::filesystem::path BundledPath() {
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
  return executable.parent_path() / "data" / "homophone-notes.tsv";
}
}  // namespace

HomophoneNotesData::HomophoneNotesData(std::filesystem::path path) : path_(std::move(path)) {}

std::shared_ptr<const core::HomophoneNoteIndex> HomophoneNotesData::Get(
    logging::RuntimeLogger* logger) {
  std::lock_guard lock(mutex_);
  if (attempted_) return index_;
  attempted_ = true;
  const auto path = path_.empty() ? BundledPath() : path_;
  size_t invalid = 0;
  if (!path.empty()) {
    std::ifstream stream(path, std::ios::binary);
    if (stream) {
      try {
        // libstdc++ opens a directory successfully and throws while reading it.
        const std::string text((std::istreambuf_iterator<char>(stream)), {});
        if (!stream.bad()) {
          auto index = std::make_shared<core::HomophoneNoteIndex>();
          invalid = index->Parse(text);
          index_ = std::move(index);
        }
      } catch (const std::exception&) {
        index_.reset();
      }
    }
  }
  if (!index_ || invalid != 0) {
    // Section 9: counts only; no readings, surfaces, notes or the path.
    if (logger)
      logger->Log(logging::RuntimeLogLevel::Warn, "homophone-notes-load-warning",
                  {{"loaded", static_cast<bool>(index_)},
                   {"invalid_rows", static_cast<uint64_t>(invalid)}});
  }
  return index_;
}
}  // namespace azookey::host
