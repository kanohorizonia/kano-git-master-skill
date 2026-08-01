#include "tui_history_lifecycle.hpp"

#include "shell_executor.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <iterator>

namespace kano::git::commands {
namespace {

constexpr unsigned int kHistoryProbeTimeoutMs = 5000;

auto TrimHistoryText(std::string InValue) -> std::string {
    while (!InValue.empty() &&
           (InValue.back() == '\n' || InValue.back() == '\r' ||
            InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() &&
           (InValue[start] == ' ' || InValue[start] == '\t')) {
        ++start;
    }
    return InValue.substr(start);
}

auto ExecuteHistoryProbe(
    const std::filesystem::path& InRepo,
    const std::vector<std::string>& InArguments)
    -> TuiHistoryProbeResult {
    const auto result = shell::ExecuteCommand(
        "git",
        InArguments,
        shell::ExecMode::Capture,
        InRepo,
        shell::ProgressCallback{},
        kHistoryProbeTimeoutMs,
        shell::CaptureLimits{kTuiStatusMaxBytes, kTuiStatusMaxBytes});
    return {
        .exitCode = result.exitCode,
        .stdoutText = result.stdoutStr,
        .stderrText = result.stderrStr,
        .stdoutTruncated = result.stdoutTruncated,
        .stderrTruncated = result.stderrTruncated,
    };
}

auto BoundedHistoryError(
    std::string InMessage,
    const std::string& InFallback) -> std::string {
    InMessage = TrimHistoryText(std::move(InMessage));
    if (InMessage.empty()) {
        InMessage = InFallback;
    }
    if (InMessage.size() > 180) {
        InMessage.resize(180);
        InMessage += "...";
    }
    return InMessage;
}

}  // namespace

auto FetchTuiHistoryBatch(
    const std::filesystem::path& InRepo,
    const int InSkip,
    const int InMaxCount,
    const std::string& InAnchorSha,
    const TuiGitProbeControl& InControl,
    const TuiHistoryProbeExecutor& InExecutor)
    -> TuiHistoryBatchResult {
    TuiHistoryBatchResult result;
    if (InMaxCount <= 0) {
        result.reachedEnd = true;
        return result;
    }

    const auto execute = InExecutor
        ? InExecutor
        : TuiHistoryProbeExecutor{ExecuteHistoryProbe};
    result.anchorSha = InAnchorSha;
    if (result.anchorSha.empty()) {
        const std::vector<std::string> anchorArguments{
            "rev-parse", "--verify", "--quiet", "HEAD"};
        if (!TryBeginTuiGitProbe(InControl, anchorArguments)) {
            result.errorMessage = "history load cancelled";
            return result;
        }
        const auto anchor = execute(InRepo, anchorArguments);
        if (anchor.stdoutTruncated || anchor.stderrTruncated) {
            result.errorMessage =
                "history anchor output exceeded the TUI audit budget";
            return result;
        }
        if (anchor.exitCode != 0) {
            if (anchor.exitCode == 1 &&
                TrimHistoryText(anchor.stderrText).empty()) {
                result.reachedEnd = true;
                return result;
            }
            result.errorMessage = BoundedHistoryError(
                anchor.stderrText,
                "git rev-parse exited with code " +
                    std::to_string(anchor.exitCode));
            return result;
        }
        result.anchorSha = TrimHistoryText(anchor.stdoutText);
    }

    const std::vector<std::string> logArguments{
        "log",
        "-z",
        "--no-decorate",
        "--skip=" + std::to_string(std::max(0, InSkip)),
        "--max-count=" + std::to_string(InMaxCount),
        "--format=%h%x00%s%x00%ae%x00%an",
        result.anchorSha,
    };
    if (!TryBeginTuiGitProbe(InControl, logArguments)) {
        result.errorMessage = "history load cancelled";
        return result;
    }
    const auto output = execute(InRepo, logArguments);
    if (output.stdoutTruncated || output.stderrTruncated) {
        result.errorMessage =
            "history output exceeded the TUI audit budget; no partial page was accepted";
        return result;
    }
    if (output.exitCode != 0) {
        result.errorMessage = BoundedHistoryError(
            output.stderrText,
            "git log exited with code " +
                std::to_string(output.exitCode));
        return result;
    }

    std::size_t position = 0;
    while (position < output.stdoutText.size()) {
        std::array<std::string, 4> fields;
        bool completeRecord = true;
        for (auto& field : fields) {
            const auto terminator = output.stdoutText.find('\0', position);
            if (terminator == std::string::npos) {
                completeRecord = false;
                break;
            }
            field.assign(
                output.stdoutText,
                position,
                terminator - position);
            position = terminator + 1;
        }
        if (!completeRecord || fields[0].empty()) {
            result.entries.clear();
            result.errorMessage =
                "history returned a malformed NUL-delimited audit record; no partial page was accepted";
            return result;
        }
        result.entries.push_back({
            .sha = std::move(fields[0]),
            .subject = std::move(fields[1]),
            .authorName = std::move(fields[3]),
            .authorEmail = std::move(fields[2]),
        });
    }
    result.reachedEnd =
        static_cast<int>(result.entries.size()) < InMaxCount;
    return result;
}

auto ReconcileHistoryAfterRepoRefresh(
    const std::string& InActiveRepoIdentityKey,
    const std::vector<std::string>& InRefreshedRepoIdentityKeys)
    -> HistoryRefreshReconciliation {
    const auto refreshedRepo = std::find(
        InRefreshedRepoIdentityKeys.begin(),
        InRefreshedRepoIdentityKeys.end(),
        InActiveRepoIdentityKey);

    if (refreshedRepo == InRefreshedRepoIdentityKeys.end()) {
        return HistoryRefreshReconciliation{
            .newRepoIndex = std::nullopt,
            .bCloseHistory = true,
            .bCloseDetail = true,
        };
    }

    return HistoryRefreshReconciliation{
        .newRepoIndex = static_cast<std::size_t>(
            std::distance(InRefreshedRepoIdentityKeys.begin(), refreshedRepo)),
        .bCloseHistory = false,
        .bCloseDetail = true,
    };
}

auto PlanBoundedHistoryFetch(const int InPageIndex,
                             const int InPageSize,
                             const int InLoadedEntryCount) -> BoundedHistoryFetchPlan {
    const int pageIndex = std::max(0, InPageIndex);
    const int pageSize = std::max(1, InPageSize);
    const int loadedEntries = std::max(0, InLoadedEntryCount);
    const auto requiredWide =
        (static_cast<long long>(pageIndex) + 1LL) * static_cast<long long>(pageSize);
    const int requiredEntries = static_cast<int>(
        std::min(requiredWide, static_cast<long long>(INT_MAX)));
    if (loadedEntries >= requiredEntries) {
        return {
            .requiredEntries = requiredEntries,
            .fetchCount = 0,
        };
    }

    const auto missingEntries =
        static_cast<long long>(requiredEntries) - static_cast<long long>(loadedEntries);
    const auto desiredWithLookAhead = missingEntries + 1LL;
    const auto boundedCount = std::min(
        desiredWithLookAhead,
        static_cast<long long>(pageSize) + 1LL);
    return {
        .requiredEntries = requiredEntries,
        .fetchCount = static_cast<int>(boundedCount),
    };
}

auto BeginHistoryPaging(const int InViewportPageSize)
    -> HistoryPagingLifecycleState {
    return {
        .pageSize = std::max(5, InViewportPageSize),
        .pageIndex = 0,
        .selectedLine = 0,
    };
}

auto ReconcileHistoryPagingAfterResize(
    const HistoryPagingLifecycleState& InState,
    const int InViewportPageSize) -> HistoryPagingLifecycleState {
    (void)InViewportPageSize;
    return {
        .pageSize = std::max(5, InState.pageSize),
        .pageIndex = std::max(0, InState.pageIndex),
        .selectedLine = std::max(0, InState.selectedLine),
    };
}

}  // namespace kano::git::commands
