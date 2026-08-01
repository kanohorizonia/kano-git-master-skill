#include "tui_async_lifecycle.hpp"

#include <limits>

namespace kano::git::commands {

auto TryBeginTuiAsyncOperation(
    TuiAsyncLifecycleState& InOutState,
    const TuiAsyncSurface InSurface,
    const bool bInCancellable) -> std::optional<std::uint64_t> {
    if (InOutState.bBusy) {
        return std::nullopt;
    }

    const auto generation = InOutState.nextGeneration;
    InOutState.nextGeneration =
        generation == std::numeric_limits<std::uint64_t>::max()
        ? 1
        : generation + 1;
    InOutState.activeGeneration = generation;
    InOutState.activeSurface = InSurface;
    InOutState.bBusy = true;
    InOutState.bCancellable = bInCancellable;
    InOutState.bSurfaceDismissed = false;
    InOutState.bExitAfterCompletion = false;
    return generation;
}

auto IsCurrentTuiAsyncOperation(
    const TuiAsyncLifecycleState& InState,
    const std::uint64_t InGeneration) -> bool {
    return InState.bBusy && InGeneration != 0 &&
        InState.activeGeneration == InGeneration;
}

auto DismissTuiAsyncSurface(
    TuiAsyncLifecycleState& InOutState,
    const TuiAsyncSurface InSurface) -> bool {
    if (!InOutState.bBusy ||
        InSurface == TuiAsyncSurface::None ||
        InOutState.activeSurface != InSurface) {
        return false;
    }
    InOutState.bSurfaceDismissed = true;
    return true;
}

auto CancelTuiAsyncSurface(
    TuiAsyncLifecycleState& InOutState,
    const TuiAsyncSurface InSurface) -> bool {
    if (!InOutState.bBusy ||
        !InOutState.bCancellable ||
        InSurface == TuiAsyncSurface::None ||
        InOutState.activeSurface != InSurface) {
        return false;
    }
    InOutState.bSurfaceDismissed = true;
    return true;
}

auto RequestTuiAsyncExit(TuiAsyncLifecycleState& InOutState)
    -> TuiAsyncExitDecision {
    if (!InOutState.bBusy) {
        return {.bExitNow = true};
    }
    InOutState.bExitAfterCompletion = true;
    return {
        .bExitNow = false,
        .bRequestCancellation = InOutState.bCancellable,
    };
}

auto CompleteTuiAsyncOperation(
    TuiAsyncLifecycleState& InOutState,
    const std::uint64_t InGeneration) -> TuiAsyncCompletionDecision {
    if (!IsCurrentTuiAsyncOperation(InOutState, InGeneration)) {
        return {};
    }

    const TuiAsyncCompletionDecision decision{
        .bMatchesActive = true,
        .bApplyPayload = true,
        .bPresentSurface =
            InOutState.activeSurface != TuiAsyncSurface::None &&
            !InOutState.bSurfaceDismissed,
        .bExitNow = InOutState.bExitAfterCompletion,
    };
    InOutState.activeGeneration = 0;
    InOutState.activeSurface = TuiAsyncSurface::None;
    InOutState.bBusy = false;
    InOutState.bCancellable = false;
    InOutState.bSurfaceDismissed = false;
    InOutState.bExitAfterCompletion = false;
    return decision;
}

}  // namespace kano::git::commands
