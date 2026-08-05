#pragma once

#include <cstdint>
#include <span>

namespace azookey::compat_test {

enum class TargetSurfaceOwnership {
  None,
  Window,
  DocumentTab,
};

enum class TargetSurfaceFailure {
  None,
  NewWindowNotFound,
  DocumentOwnershipNotEstablished,
};

struct TargetWindowObservation {
  std::uintptr_t window_id{0};
  std::uint32_t process_id{0};
  bool existed_before{false};
  bool executable_matches{false};
  bool temporary_document_selected{false};
};

struct TargetSurfaceSelection {
  std::uintptr_t window_id{0};
  TargetSurfaceOwnership ownership{TargetSurfaceOwnership::None};
  TargetSurfaceFailure failure{TargetSurfaceFailure::None};
  bool window_process_is_launched_process{false};
};

TargetSurfaceSelection SelectTargetSurface(std::span<const TargetWindowObservation> observations,
                                           std::uint32_t launched_process_id,
                                           bool allow_reused_window_for_temporary_document);

const char* TargetSurfaceFailureReason(TargetSurfaceFailure failure);

}  // namespace azookey::compat_test
