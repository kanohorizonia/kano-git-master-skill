#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace kano::git::commands {

inline constexpr std::size_t kTuiStatusMaxBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kTuiStatusMaxEntries = 4096U;
inline constexpr std::size_t kTuiFilePatchMaxBytes = 20000U;
inline constexpr std::size_t kTuiDetailPatchFetchLimit = 8U;
inline constexpr std::size_t kTuiDetailPatchMaxBytes =
    kTuiFilePatchMaxBytes * kTuiDetailPatchFetchLimit;

struct TuiPorcelainParseLimits {
    std::size_t maxBytes = kTuiStatusMaxBytes;
    std::size_t maxEntries = kTuiStatusMaxEntries;
};

struct TuiPorcelainPath {
    char indexStatus = ' ';
    char worktreeStatus = ' ';
    std::string path;
    std::string previousPath;
};

struct TuiPorcelainParseResult {
    std::vector<TuiPorcelainPath> entries;
    bool truncated = false;
    bool malformed = false;
};

struct TuiDetailPatchBudget {
    std::size_t startedFetches = 0;
};

/// Parse `git status --porcelain=v1 -z` without decoding, trimming, or
/// line-splitting path identity. For rename/copy records Git emits the new path
/// first and the old path as the following NUL-delimited field.
auto ParseTuiPorcelainV1Z(
    std::string_view InRaw,
    TuiPorcelainParseLimits InLimits = {}) -> TuiPorcelainParseResult;

/// Escape control bytes only for display. The original parsed path remains the
/// authoritative pathspec argument.
auto EscapeTuiPathForDisplay(std::string_view InPath) -> std::string;

/// Reserve one path-specific patch fetch. Callers must reserve before starting
/// Git so a single detail selection cannot exceed the documented limit.
auto TryStartTuiDetailPatchFetch(TuiDetailPatchBudget& InOutBudget) noexcept
    -> bool;

struct HistoryChangedPath {
    std::string status;
    std::string path;
    std::string previousPath;
};

struct HistoryNameStatusParseResult {
    std::vector<HistoryChangedPath> entries;
    bool truncated = false;
    bool malformed = false;
};

/// Optional cancellation and launch observation for a bounded sequence of Git
/// probes. `isCancelled` is checked immediately before every subprocess
/// launch. `onLaunch` exists so deterministic tests can prove that no later
/// probe starts after cancellation.
struct TuiGitProbeControl {
    std::function<bool()> isCancelled;
    std::function<void(const std::vector<std::string>&)> onLaunch;
};

/// Atomically apply the shared pre-launch cancellation/observation contract.
/// Every multi-probe TUI reader must call this immediately before spawning.
auto TryBeginTuiGitProbe(
    const TuiGitProbeControl& InControl,
    const std::vector<std::string>& InArguments) -> bool;

/// Parse `git show -z --name-status` output without treating path bytes as
/// line-oriented text. Rename/copy records contain two path fields.
auto ParseHistoryNameStatusZ(
    std::string_view InRaw,
    TuiPorcelainParseLimits InLimits = {})
    -> HistoryNameStatusParseResult;

auto FetchCommitDetail(const std::filesystem::path& InRepo,
                       const std::string& InSha,
                       int InMode,
                       const TuiGitProbeControl& InControl = {}) -> std::string;
auto FetchCommitFilePatch(const std::filesystem::path& InRepo,
                          const std::string& InSha,
                          const std::string& InPatchPath,
                          const std::string& InPatchPathAlt = {},
                          const TuiGitProbeControl& InControl = {}) -> std::string;
auto FetchWorkingTreeFilePatch(const std::filesystem::path& InRepo,
                               const std::string& InPatchPath,
                               const std::string& InPatchPathAlt = {},
                               const TuiGitProbeControl& InControl = {}) -> std::string;
auto FetchWorkingTreeDetail(const std::filesystem::path& InRepo,
                            int InMode,
                            const TuiGitProbeControl& InControl = {}) -> std::string;

}
