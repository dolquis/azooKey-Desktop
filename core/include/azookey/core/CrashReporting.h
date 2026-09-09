#pragma once

#include <cstdint>
#include <filesystem>

namespace azookey::core {
enum class CrashConsent { Off, Local };
enum class CrashModule { Host, Settings };
enum class CrashStatus { Disabled, Ready, DirectoryUnavailable, WriteFailed, Unsupported };

// Process-wide, opt-in metadata collection. Never install this in the TIP DLL.
class CrashReporting {
 public:
  static std::filesystem::path DefaultDirectory() noexcept;
  static void Initialize(CrashModule module, CrashConsent consent,
                         const std::filesystem::path& directory = {}) noexcept;
  static void SetConsent(CrashConsent consent) noexcept;
  static void Shutdown() noexcept;
  static CrashStatus Status() noexcept;
  // Worker/normal-thread writer; never call directly from a faulting thread.
  // Writes only the allowlisted numeric exception fields, never process memory.
  static bool WriteReport(std::uint32_t code, std::uint64_t address,
                          std::uint32_t thread_id) noexcept;
};
}  // namespace azookey::core
