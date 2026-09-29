#pragma once

#include <cstdint>
#include <optional>

namespace kano::git::shell {
struct ExecResult;
}

namespace kano::git::commands {

enum class TuiAsyncSurface {
    None,
    HistoryPage,
    HistoryDetail,
    Discover,
    Preview,
};

enum class TuiAsyncProcessCompletion {
    Completed,
    Cancelled,
    Failed,
};

struct TuiAsyncLifecycleState {
    std::uint64_t nextGeneration = 1;
    std::uint64_t activeGeneration = 0;
    TuiAsyncSurface activeSurface = TuiAsyncSurface::None;
    bool bBusy = false;
    bool bCancellable = false;
    bool bSurfaceDismissed = false;
    bool bExitAfterCompletion = false;
};

struct TuiAsyncExitDecision {
    bool bExitNow = false;
    bool bRequestCancellation = false;
};

struct TuiAsyncCompletionDecision {
    bool bMatchesActive = false;
    bool bApplyPayload = false;
    bool bPresentSurface = false;
    bool bExitNow = false;
};

[[nodiscard]] auto ClassifyTuiAsyncProcessResult(
    const kano::git::shell::ExecResult& InResult)
    -> TuiAsyncProcessCompletion;

[[nodiscard]] auto TryBeginTuiAsyncOperation(
    TuiAsyncLifecycleState& InOutState,
    TuiAsyncSurface InSurface,
    bool bInCancellable) -> std::optional<std::uint64_t>;

[[nodiscard]] auto IsCurrentTuiAsyncOperation(
    const TuiAsyncLifecycleState& InState,
    std::uint64_t InGeneration) -> bool;

[[nodiscard]] auto DismissTuiAsyncSurface(
    TuiAsyncLifecycleState& InOutState,
    TuiAsyncSurface InSurface) -> bool;

[[nodiscard]] auto CancelTuiAsyncSurface(
    TuiAsyncLifecycleState& InOutState,
    TuiAsyncSurface InSurface) -> bool;

[[nodiscard]] auto RequestTuiAsyncExit(
    TuiAsyncLifecycleState& InOutState) -> TuiAsyncExitDecision;

[[nodiscard]] auto CompleteTuiAsyncOperation(
    TuiAsyncLifecycleState& InOutState,
    std::uint64_t InGeneration) -> TuiAsyncCompletionDecision;

}  // namespace kano::git::commands
