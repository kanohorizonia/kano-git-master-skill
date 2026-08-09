#pragma once

#include "tui_history_patch.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kano::git::commands {

struct TuiHistoryEntry {
    bool isDirtyWorkingTree = false;
    std::string sha;
    std::string subject;
    std::string authorName;
    std::string authorEmail;
    int globalIndex = 0;
    int totalCount = 0;
    std::string displayLine;
};

struct TuiHistoryBatchResult {
    std::vector<TuiHistoryEntry> entries;
    bool reachedEnd = false;
    std::string errorMessage;
    std::string anchorSha;
};

enum class TuiHistoryPageOrder {
    NewestFirst,
    OldestFirst,
    MatchesFirst,
};

enum class TuiHistoryPageDirection {
    Newer,
    Older,
};

/// Return the canonical audit label for ordering one already-bounded page.
/// Labels are deliberately page-scoped and never imply repository-global
/// chronological ordering.
[[nodiscard]] auto TuiHistoryPageOrderName(
    TuiHistoryPageOrder InOrder) noexcept -> std::string_view;

[[nodiscard]] auto NextTuiHistoryPageOrder(
    TuiHistoryPageOrder InOrder) noexcept -> TuiHistoryPageOrder;

/// Describe a bounded page boundary without making claims about which commit
/// occupies the first or last visible row after page-local reordering.
[[nodiscard]] auto TuiHistoryPageBoundaryMessage(
    TuiHistoryPageDirection InDirection) noexcept -> std::string_view;

/// Reorder only the supplied bounded page. NewestFirst preserves the Git-log
/// order, OldestFirst reverses this page, and MatchesFirst stably promotes
/// entries matching the current query without changing page membership.
[[nodiscard]] auto OrderTuiHistoryPage(
    std::vector<TuiHistoryEntry> InEntries,
    TuiHistoryPageOrder InOrder,
    std::string_view InSearchQuery) -> std::vector<TuiHistoryEntry>;

struct TuiHistoryProbeResult {
    int exitCode = 0;
    std::string stdoutText;
    std::string stderrText;
    bool stdoutTruncated = false;
    bool stderrTruncated = false;
};

using TuiHistoryProbeExecutor = std::function<TuiHistoryProbeResult(
    const std::filesystem::path&,
    const std::vector<std::string>&)>;

/// Fetch one bounded, HEAD-anchored history batch. Cancellation is checked
/// exactly once immediately before each probe, including between the optional
/// anchor lookup and the log query. The executor hook provides deterministic
/// launch-count tests without starting subprocesses.
auto FetchTuiHistoryBatch(
    const std::filesystem::path& InRepo,
    int InSkip,
    int InMaxCount,
    const std::string& InAnchorSha,
    const TuiGitProbeControl& InControl = {},
    const TuiHistoryProbeExecutor& InExecutor = {})
    -> TuiHistoryBatchResult;

struct HistoryRefreshReconciliation {
    std::optional<std::size_t> newRepoIndex;
    bool bCloseHistory = false;
    bool bCloseDetail = false;
};

struct BoundedHistoryFetchPlan {
    int requiredEntries = 0;
    int fetchCount = 0;
};

struct HistoryPagingLifecycleState {
    int pageSize = 20;
    int pageIndex = 0;
    int selectedLine = 0;
};

/// Reconcile an active history repository after the repository list is refreshed.
///
/// The caller owns repository identity resolution. This helper compares stable
/// cached identity keys without touching the filesystem.
auto ReconcileHistoryAfterRepoRefresh(
    const std::string& InActiveRepoIdentityKey,
    const std::vector<std::string>& InRefreshedRepoIdentityKeys)
    -> HistoryRefreshReconciliation;

/// Plan one bounded history fetch. The returned fetch count never exceeds one
/// visible page plus a look-ahead row, even if stale UI state requests a far
/// page after cache invalidation.
auto PlanBoundedHistoryFetch(int InPageIndex,
                             int InPageSize,
                             int InLoadedEntryCount) -> BoundedHistoryFetchPlan;

/// Capture page geometry when history opens. The page size remains fixed for
/// that history session so a terminal resize cannot reinterpret cached page
/// offsets.
auto BeginHistoryPaging(int InViewportPageSize)
    -> HistoryPagingLifecycleState;

/// Reconcile a resize without changing the page boundary captured at open.
/// Closing and reopening history is the explicit boundary for adopting the
/// new viewport page size.
auto ReconcileHistoryPagingAfterResize(
    const HistoryPagingLifecycleState& InState,
    int InViewportPageSize) -> HistoryPagingLifecycleState;

}  // namespace kano::git::commands
