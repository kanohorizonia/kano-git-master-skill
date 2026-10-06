#include <catch2/catch_test_macros.hpp>

#include "shell_executor.hpp"
#include "tui_async_lifecycle.hpp"

using kano::git::commands::CompleteTuiAsyncOperation;
using kano::git::commands::CancelTuiAsyncSurface;
using kano::git::commands::ClassifyTuiAsyncProcessResult;
using kano::git::commands::DismissTuiAsyncSurface;
using kano::git::commands::IsCurrentTuiAsyncOperation;
using kano::git::commands::RequestTuiAsyncExit;
using kano::git::commands::TryBeginTuiAsyncOperation;
using kano::git::commands::TuiAsyncLifecycleState;
using kano::git::commands::TuiAsyncProcessCompletion;
using kano::git::commands::TuiAsyncSurface;
using kano::git::shell::ExecOutcome;
using kano::git::shell::ExecResult;

TEST_CASE(
    "dismissed TUI async detail completion fills cache without reopening",
    "[unit][tui_async_lifecycle][KG-TSK-0070]") {
    TuiAsyncLifecycleState state;
    const auto generation = TryBeginTuiAsyncOperation(
        state,
        TuiAsyncSurface::HistoryDetail,
        true);
    REQUIRE(generation.has_value());
    REQUIRE(DismissTuiAsyncSurface(
        state,
        TuiAsyncSurface::HistoryDetail));

    const auto completion =
        CompleteTuiAsyncOperation(state, *generation);
    REQUIRE(completion.bMatchesActive);
    REQUIRE(completion.bApplyPayload);
    REQUIRE_FALSE(completion.bPresentSurface);
    REQUIRE_FALSE(completion.bExitNow);
}

TEST_CASE(
    "TUI async surface cancellation dismisses only matching cancellable reads",
    "[unit][tui_async_lifecycle][KG-BUG-0091]") {
    TuiAsyncLifecycleState state;
    const auto generation = TryBeginTuiAsyncOperation(
        state,
        TuiAsyncSurface::HistoryPage,
        true);
    REQUIRE(generation.has_value());
    REQUIRE_FALSE(CancelTuiAsyncSurface(
        state,
        TuiAsyncSurface::HistoryDetail));
    REQUIRE(CancelTuiAsyncSurface(
        state,
        TuiAsyncSurface::HistoryPage));

    const auto completion = CompleteTuiAsyncOperation(state, *generation);
    REQUIRE(completion.bMatchesActive);
    REQUIRE(completion.bApplyPayload);
    REQUIRE_FALSE(completion.bPresentSurface);

    const auto nonCancellable = TryBeginTuiAsyncOperation(
        state,
        TuiAsyncSurface::Preview,
        false);
    REQUIRE(nonCancellable.has_value());
    REQUIRE_FALSE(CancelTuiAsyncSurface(
        state,
        TuiAsyncSurface::Preview));
}

TEST_CASE(
    "TUI async exit requests cancellation only for cancellable work",
    "[unit][tui_async_lifecycle][tui_pr_focus][KG-TSK-0070]") {
    TuiAsyncLifecycleState state;
    const auto readGeneration = TryBeginTuiAsyncOperation(
        state,
        TuiAsyncSurface::HistoryPage,
        true);
    REQUIRE(readGeneration.has_value());
    const auto cancellableExit = RequestTuiAsyncExit(state);
    REQUIRE_FALSE(cancellableExit.bExitNow);
    REQUIRE(cancellableExit.bRequestCancellation);
    REQUIRE(CompleteTuiAsyncOperation(state, *readGeneration).bExitNow);

    const auto writeGeneration = TryBeginTuiAsyncOperation(
        state,
        TuiAsyncSurface::Preview,
        false);
    REQUIRE(writeGeneration.has_value());
    const auto safeExit = RequestTuiAsyncExit(state);
    REQUIRE_FALSE(safeExit.bExitNow);
    REQUIRE_FALSE(safeExit.bRequestCancellation);
    REQUIRE(CompleteTuiAsyncOperation(state, *writeGeneration).bExitNow);
}

TEST_CASE(
    "TUI async stale and duplicate completions cannot clear newer work",
    "[unit][tui_async_lifecycle][KG-TSK-0070]") {
    TuiAsyncLifecycleState state;
    const auto first = TryBeginTuiAsyncOperation(
        state,
        TuiAsyncSurface::Discover,
        true);
    REQUIRE(first.has_value());
    REQUIRE(CompleteTuiAsyncOperation(state, *first).bMatchesActive);
    REQUIRE_FALSE(
        CompleteTuiAsyncOperation(state, *first).bMatchesActive);

    const auto second = TryBeginTuiAsyncOperation(
        state,
        TuiAsyncSurface::Preview,
        false);
    REQUIRE(second.has_value());
    REQUIRE_FALSE(
        CompleteTuiAsyncOperation(state, *first).bMatchesActive);
    REQUIRE(IsCurrentTuiAsyncOperation(state, *second));
    REQUIRE(CompleteTuiAsyncOperation(state, *second).bMatchesActive);
}

TEST_CASE(
    "TUI async idle exit is immediate and a busy begin is rejected",
    "[unit][tui_async_lifecycle][KG-TSK-0070]") {
    TuiAsyncLifecycleState state;
    REQUIRE(RequestTuiAsyncExit(state).bExitNow);

    REQUIRE(TryBeginTuiAsyncOperation(
                state,
                TuiAsyncSurface::None,
                false)
                .has_value());
    REQUIRE_FALSE(
        TryBeginTuiAsyncOperation(
            state,
            TuiAsyncSurface::Discover,
            true)
            .has_value());
}

TEST_CASE(
    "TUI async process completion uses the typed cancellation outcome",
    "[unit][tui_async_lifecycle][cancellation][KOG-BUG-0107]") {
    const ExecResult cancelled{
        .exitCode = 143,
        .stderrStr = "owned process tree stopped",
        .outcome = ExecOutcome::Cancelled,
    };
    REQUIRE(
        ClassifyTuiAsyncProcessResult(cancelled) ==
        TuiAsyncProcessCompletion::Cancelled);

    const ExecResult misleadingFailure{
        .exitCode = 1,
        .stderrStr = "operation was not cancelled",
        .outcome = ExecOutcome::Completed,
    };
    REQUIRE(
        ClassifyTuiAsyncProcessResult(misleadingFailure) ==
        TuiAsyncProcessCompletion::Failed);

    const ExecResult timedOut{
        .exitCode = 124,
        .stderrStr = "timeout",
        .outcome = ExecOutcome::TimedOut,
    };
    REQUIRE(
        ClassifyTuiAsyncProcessResult(timedOut) ==
        TuiAsyncProcessCompletion::Failed);

    const ExecResult completed{
        .exitCode = 0,
        .outcome = ExecOutcome::Completed,
    };
    REQUIRE(
        ClassifyTuiAsyncProcessResult(completed) ==
        TuiAsyncProcessCompletion::Completed);
}
