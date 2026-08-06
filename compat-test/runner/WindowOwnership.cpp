#include "runner/WindowOwnership.h"

#include <algorithm>
#include <iterator>

namespace azookey::compat_test {
namespace {

TargetSurfaceSelection SelectNewWindow(std::span<const TargetWindowObservation> observations,
                                       std::uint32_t launched_process_id) {
  const auto select = [&](const auto& predicate) -> TargetSurfaceSelection {
    const auto match = std::ranges::find_if(observations, predicate);
    if (match == observations.end()) return {};
    return {.window_id = match->window_id,
            .ownership = TargetSurfaceOwnership::Window,
            .failure = TargetSurfaceFailure::None,
            .window_process_is_launched_process = match->process_id == launched_process_id};
  };

  auto selection = select([&](const TargetWindowObservation& observation) {
    return !observation.existed_before && observation.process_id == launched_process_id;
  });
  if (selection.ownership != TargetSurfaceOwnership::None) return selection;

  return select([](const TargetWindowObservation& observation) {
    return !observation.existed_before && observation.executable_matches;
  });
}

}  // namespace

TargetSurfaceSelection SelectTargetSurface(std::span<const TargetWindowObservation> observations,
                                           std::uint32_t launched_process_id,
                                           bool allow_reused_window_for_temporary_document) {
  auto selection = SelectNewWindow(observations, launched_process_id);
  if (selection.ownership != TargetSurfaceOwnership::None) return selection;

  if (allow_reused_window_for_temporary_document) {
    const auto is_owned_document = [](const TargetWindowObservation& observation) {
      return observation.existed_before && observation.executable_matches &&
             observation.temporary_document_selected;
    };
    const auto first = std::ranges::find_if(observations, is_owned_document);
    if (first != observations.end() &&
        std::ranges::find_if(std::next(first), observations.end(), is_owned_document) ==
            observations.end()) {
      return {.window_id = first->window_id,
              .ownership = TargetSurfaceOwnership::DocumentTab,
              .failure = TargetSurfaceFailure::None,
              .window_process_is_launched_process = first->process_id == launched_process_id};
    }
    const bool has_matching_existing_window =
        std::ranges::any_of(observations, [](const TargetWindowObservation& observation) {
          return observation.existed_before && observation.executable_matches;
        });
    if (has_matching_existing_window) {
      return {.failure = TargetSurfaceFailure::DocumentOwnershipNotEstablished};
    }
  }

  return {.failure = TargetSurfaceFailure::NewWindowNotFound};
}

const char* TargetSurfaceFailureReason(TargetSurfaceFailure failure) {
  switch (failure) {
    case TargetSurfaceFailure::None:
      return "";
    case TargetSurfaceFailure::NewWindowNotFound:
      return "new-target-window-not-found";
    case TargetSurfaceFailure::DocumentOwnershipNotEstablished:
      return "target-document-ownership-not-established";
  }
  return "target-surface-selection-failed";
}

}  // namespace azookey::compat_test
