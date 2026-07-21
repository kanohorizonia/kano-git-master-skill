#include "repo_health.hpp"

#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace kano::git::workspace {
namespace {

auto Trim(std::string InValue) -> std::string {
    while (!InValue.empty() && (InValue.back() == '\n' || InValue.back() == '\r' || InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() && (InValue[start] == ' ' || InValue[start] == '\t')) {
        start += 1;
    }
    return InValue.substr(start);
}

auto SplitLines(const std::string& InText) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::istringstream iss(InText);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (!line.empty()) {
            out.push_back(std::move(line));
        }
    }
    return out;
}

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto RepoPathKey(const std::filesystem::path& InPath) -> std::string {
    std::error_code ec;
    auto path = std::filesystem::weakly_canonical(InPath, ec);
    if (ec) {
        path = std::filesystem::absolute(InPath, ec);
    }
    if (ec) {
        path = InPath;
    }
    auto out = path.lexically_normal().generic_string();
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
#if defined(_WIN32)
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    return out;
}

auto AddUnique(std::vector<std::string>* IoValues, const std::string& InValue) -> void {
    if (IoValues == nullptr || InValue.empty()) {
        return;
    }
    if (std::find(IoValues->begin(), IoValues->end(), InValue) == IoValues->end()) {
        IoValues->push_back(InValue);
    }
}

auto AddBlocker(RepoHealth* IoHealth,
                RepoBlockerKind InKind,
                const std::string& InDetail) -> void {
    if (IoHealth == nullptr) {
        return;
    }
    const auto reason = RepoBlockerKindToReasonCode(InKind);
    const auto it = std::find_if(IoHealth->blockers.begin(), IoHealth->blockers.end(), [&](const RepoBlocker& blocker) {
        return blocker.reasonCode == reason && blocker.detail == InDetail;
    });
    if (it != IoHealth->blockers.end()) {
        return;
    }
    IoHealth->blockers.push_back(RepoBlocker{InKind, reason, InDetail});
    AddUnique(&IoHealth->statusFlags, reason);
}

auto HasGitPath(const std::filesystem::path& InRepo, const std::string& InGitPath) -> bool {
    const auto out = GitCapture(InRepo, {"rev-parse", "--git-path", InGitPath});
    if (out.exitCode != 0) {
        return false;
    }
    const auto value = Trim(out.stdoutStr);
    if (value.empty()) {
        return false;
    }
    return std::filesystem::exists(std::filesystem::path(value));
}

auto ParseStatusFlag(const std::string& InLine) -> std::string {
    if (InLine.size() < 2) {
        return InLine;
    }
    return InLine.substr(0, 2);
}

auto IsInternalArtifactStatusLine(const std::string& InLine) -> bool {
    if (InLine.size() <= 3) {
        return false;
    }

    auto path = Trim(InLine.substr(3));
    if (const auto arrow = path.rfind(" -> "); arrow != std::string::npos) {
        path = Trim(path.substr(arrow + 4));
    }
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
        path = path.substr(1, path.size() - 2);
    }
    std::replace(path.begin(), path.end(), '\\', '/');

    const auto matchesReservedRoot = [&path](const std::string_view InRoot) {
        return path == InRoot || path.starts_with(std::string(InRoot) + "/");
    };
    return matchesReservedRoot(".kano/cache") ||
           matchesReservedRoot(".kano/tmp") ||
           matchesReservedRoot(".kano/launcher") ||
           matchesReservedRoot(".kano/reports") ||
           matchesReservedRoot(".kano/git") ||
           matchesReservedRoot(".sisyphus");
}

auto StatusHasUnmergedFlag(const std::string& InFlag) -> bool {
    if (InFlag.size() < 2) {
        return false;
    }
    if (InFlag.find('U') != std::string::npos) {
        return true;
    }
    return InFlag == "AA" || InFlag == "DD";
}

auto FirstLine(const std::string& InText) -> std::string {
    std::istringstream iss(InText);
    std::string line;
    if (!std::getline(iss, line)) {
        return {};
    }
    return Trim(line);
}

auto ExtractMissingSubmoduleMappingPath(const std::string& InText) -> std::string {
    constexpr std::string_view kNeedle = "no submodule mapping found in .gitmodules for path '";
    const auto pos = InText.find(kNeedle);
    if (pos == std::string::npos) {
        return {};
    }
    const auto begin = pos + kNeedle.size();
    const auto end = InText.find('\'', begin);
    if (end == std::string::npos || end <= begin) {
        return {};
    }
    return Trim(InText.substr(begin, end - begin));
}

auto ParseSubmoduleStatusText(const std::string& InText) -> std::vector<ParsedSubmoduleStatusLine> {
    std::vector<ParsedSubmoduleStatusLine> out;
    for (const auto& line : SplitLines(InText)) {
        auto parsed = ParseSubmoduleStatusLine(line);
        if (parsed.valid) {
            out.push_back(std::move(parsed));
        }
    }
    return out;
}

auto ParseSubmodulePathToNameMap(const std::filesystem::path& InRepo) -> std::unordered_map<std::string, std::string> {
    std::unordered_map<std::string, std::string> byPath;
    const auto out = GitCapture(InRepo, {"config", "--file", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
    if (out.exitCode != 0) {
        return byPath;
    }
    for (const auto& line : SplitLines(out.stdoutStr)) {
        const auto split = line.find(' ');
        if (split == std::string::npos) {
            continue;
        }
        const auto key = Trim(line.substr(0, split));
        const auto path = Trim(line.substr(split + 1));
        const auto keyPrefix = std::string{"submodule."};
        const auto keySuffix = std::string{".path"};
        if (!key.starts_with(keyPrefix) || !key.ends_with(keySuffix) || key.size() <= keyPrefix.size() + keySuffix.size()) {
            continue;
        }
        const auto name = key.substr(keyPrefix.size(), key.size() - keyPrefix.size() - keySuffix.size());
        if (!name.empty() && !path.empty()) {
            byPath.emplace(path, name);
        }
    }
    return byPath;
}

auto ExpectedSubmoduleBranch(const std::filesystem::path& InRepo,
                             const std::unordered_map<std::string, std::string>& InPathToName,
                             const std::string& InPath) -> std::string {
    const auto it = InPathToName.find(InPath);
    if (it == InPathToName.end()) {
        return {};
    }
    const auto key = std::format("submodule.{}.branch", it->second);
    const auto out = GitCapture(InRepo, {"config", "--file", ".gitmodules", "--get", key});
    if (out.exitCode != 0) {
        return {};
    }
    return Trim(out.stdoutStr);
}

} // namespace

auto RepoBlockerKindToReasonCode(const RepoBlockerKind InKind) -> std::string {
    switch (InKind) {
    case RepoBlockerKind::ActiveRebase: return "ACTIVE_REBASE";
    case RepoBlockerKind::ActiveMerge: return "ACTIVE_MERGE";
    case RepoBlockerKind::ActiveCherryPick: return "ACTIVE_CHERRY_PICK";
    case RepoBlockerKind::ActiveRevert: return "ACTIVE_REVERT";
    case RepoBlockerKind::ActiveBisect: return "ACTIVE_BISECT";
    case RepoBlockerKind::UnmergedPaths: return "UNMERGED_PATHS";
    case RepoBlockerKind::UnresolvedGitlink: return "UNRESOLVED_GITLINK";
    case RepoBlockerKind::DetachedHead: return "DETACHED_HEAD";
    case RepoBlockerKind::DirtyWorktree: return "DIRTY_WORKTREE";
    case RepoBlockerKind::DirtySubmodule: return "DIRTY_SUBMODULE";
    case RepoBlockerKind::FetchFailed: return "FETCH_FAILED";
    case RepoBlockerKind::NoUpstream: return "NO_UPSTREAM";
    case RepoBlockerKind::BranchDiverged: return "BRANCH_DIVERGED";
    case RepoBlockerKind::UnpushedCommits: return "UNPUSHED_COMMITS";
    case RepoBlockerKind::GitlinkUnreachable: return "GITLINK_UNREACHABLE";
    case RepoBlockerKind::SubmoduleStatusUnresolved: return "SUBMODULE_STATUS_UNRESOLVED";
    case RepoBlockerKind::SubmoduleMappingMissing: return "KOG_SUBMODULE_MAPPING_MISSING";
    case RepoBlockerKind::KogPlanUnauditable: return "KOG_PLAN_UNAUDITABLE";
    }
    return "KOG_PLAN_UNAUDITABLE";
}

auto ParseSubmoduleStatusLine(const std::string& InLine) -> ParsedSubmoduleStatusLine {
    ParsedSubmoduleStatusLine out;
    if (InLine.size() < 42) {
        return out;
    }
    out.marker = InLine[0];
    out.sha = InLine.substr(1, 40);
    for (const char ch : out.sha) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            out.sha.clear();
            out.marker = ' ';
            return out;
        }
    }

    auto path = Trim(InLine.substr(41));
    const auto meta = path.find(" (");
    if (meta != std::string::npos) {
        path = Trim(path.substr(0, meta));
    }
    if (path.empty()) {
        return out;
    }

    out.path = std::move(path);
    out.shaAllZero = std::all_of(out.sha.begin(), out.sha.end(), [](const char ch) { return ch == '0'; });
    out.valid = true;
    return out;
}

auto ScanRepoHealth(const std::filesystem::path& InRepo,
                    const RepoHealthOptions& InOptions) -> RepoHealth {
    RepoHealth out;

    const auto branchOut = GitCapture(InRepo, {"symbolic-ref", "--quiet", "--short", "HEAD"});
    if (branchOut.exitCode == 0) {
        out.branch = Trim(branchOut.stdoutStr);
    }
    if (out.branch.empty()) {
        out.branch = "(detached)";
        out.detachedHead = true;
        if (InOptions.blockOnDetachedHead) {
            AddBlocker(&out, RepoBlockerKind::DetachedHead, "branch context is detached");
        }
    }

    if (HasGitPath(InRepo, "rebase-merge") || HasGitPath(InRepo, "rebase-apply")) {
        AddBlocker(&out, RepoBlockerKind::ActiveRebase, "rebase operation is in progress");
    }
    if (HasGitPath(InRepo, "MERGE_HEAD")) {
        AddBlocker(&out, RepoBlockerKind::ActiveMerge, "merge operation is in progress");
    }
    if (HasGitPath(InRepo, "CHERRY_PICK_HEAD")) {
        AddBlocker(&out, RepoBlockerKind::ActiveCherryPick, "cherry-pick operation is in progress");
    }
    if (HasGitPath(InRepo, "REVERT_HEAD")) {
        AddBlocker(&out, RepoBlockerKind::ActiveRevert, "revert operation is in progress");
    }
    if (HasGitPath(InRepo, "BISECT_LOG")) {
        AddBlocker(&out, RepoBlockerKind::ActiveBisect, "bisect operation is in progress");
    }

    // Enumerate untracked files individually so internal .kano artifacts can be
    // filtered without hiding adjacent user-authored .kano configuration.
    const auto statusOut = GitCapture(InRepo, {"status", "--porcelain=v1", "--untracked-files=all"});
    bool hasStaged = false;
    bool hasModified = false;
    bool hasUntracked = false;
    if (statusOut.exitCode != 0) {
        AddBlocker(&out, RepoBlockerKind::KogPlanUnauditable, "git status failed: " + FirstLine(statusOut.stderrStr));
    } else {
        std::istringstream statusLines(statusOut.stdoutStr);
        std::string line;
        while (std::getline(statusLines, line)) {
            while (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty() || IsInternalArtifactStatusLine(line)) {
                continue;
            }
            const auto flag = ParseStatusFlag(line);
            if (StatusHasUnmergedFlag(flag)) {
                out.hasUnmergedPaths = true;
            }
            if (flag == "??") {
                hasUntracked = true;
                continue;
            }
            if (!flag.empty() && flag[0] != ' ') {
                hasStaged = true;
            }
            if (flag.size() > 1 && flag[1] != ' ') {
                hasModified = true;
            }
        }
    }

    out.hasDirtyWorktree = hasStaged || hasModified || hasUntracked;
    if (out.hasUnmergedPaths) {
        AddBlocker(&out, RepoBlockerKind::UnmergedPaths, "unmerged paths detected in working tree");
    }
    if (InOptions.blockOnDirtyWorktree && out.hasDirtyWorktree) {
        AddBlocker(&out, RepoBlockerKind::DirtyWorktree, "dirty worktree cannot be auto-handled by current policy");
    }

    const auto upstreamOut = GitCapture(InRepo, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"});
    if (upstreamOut.exitCode == 0) {
        out.upstream = Trim(upstreamOut.stdoutStr);
    }
    if (out.upstream.empty() && InOptions.blockOnNoUpstream) {
        AddBlocker(&out, RepoBlockerKind::NoUpstream, "branch has no upstream tracking ref");
    }

    if (!out.upstream.empty()) {
        const auto aheadBehindOut = GitCapture(InRepo, {"rev-list", "--left-right", "--count", out.upstream + "...HEAD"});
        if (aheadBehindOut.exitCode == 0) {
            std::istringstream iss(aheadBehindOut.stdoutStr);
            int behind = 0;
            int ahead = 0;
            if (iss >> behind >> ahead) {
                out.ahead = ahead;
                out.behind = behind;
            }
        }
    }

    out.hasUnpushedCommits = out.ahead > 0;
    if (out.hasUnpushedCommits) {
        AddUnique(&out.statusFlags, "UNPUSHED_COMMITS");
        if (InOptions.blockOnUnpushedCommits) {
            AddBlocker(&out, RepoBlockerKind::UnpushedCommits, "local commits are ahead of upstream");
        }
    }
    if (out.ahead > 0 && out.behind > 0) {
        AddBlocker(&out, RepoBlockerKind::BranchDiverged,
                   std::format("local branch diverged from upstream (ahead={}, behind={})", out.ahead, out.behind));
    }

    if (InOptions.checkFetchRemotes) {
        const auto remotesOut = GitCapture(InRepo, {"remote"});
        if (remotesOut.exitCode != 0) {
            AddBlocker(&out, RepoBlockerKind::KogPlanUnauditable, "git remote failed: " + FirstLine(remotesOut.stderrStr));
        } else {
            auto remotes = SplitLines(remotesOut.stdoutStr);
            if (!InOptions.fetchRemoteOnly.empty()) {
                const auto selected = Trim(InOptions.fetchRemoteOnly);
                const auto found = std::find(remotes.begin(), remotes.end(), selected) != remotes.end();
                if (!found) {
                    AddBlocker(&out, RepoBlockerKind::FetchFailed,
                               std::format("remote={} is not configured locally", selected));
                    remotes.clear();
                } else {
                    remotes = {selected};
                }
            }
            for (const auto& remote : remotes) {
                std::vector<std::string> fetchArgs{"fetch", remote, "--prune", "--tags"};
                if (InOptions.fetchDryRun) {
                    fetchArgs.push_back("--dry-run");
                }
                const auto fetchOut = GitCapture(InRepo, fetchArgs);
                if (fetchOut.exitCode != 0) {
                    AddBlocker(&out, RepoBlockerKind::FetchFailed,
                               std::format("remote={} fetch failed: {}", remote, FirstLine(fetchOut.stderrStr + "\n" + fetchOut.stdoutStr)));
                }
            }
        }
    }

    if (InOptions.checkSubmoduleStatus) {
        std::string submoduleStatusText;
        if (const char* overrideStatus = std::getenv("KOG_TEST_HEALTH_SUBMODULE_STATUS"); overrideStatus != nullptr && std::string(overrideStatus).size() > 0) {
            submoduleStatusText = overrideStatus;
        } else {
            const auto submoduleStatusOut = GitCapture(InRepo, {"submodule", "status", "--recursive"});
            if (submoduleStatusOut.exitCode == 0) {
                submoduleStatusText = submoduleStatusOut.stdoutStr;
            } else {
                const auto missingPath = ExtractMissingSubmoduleMappingPath(submoduleStatusOut.stderrStr);
                if (!missingPath.empty()) {
                    const bool managedByGraph = InOptions.managedSubmodulePaths.contains(missingPath);
                    if (InOptions.strictSubmoduleMappings || managedByGraph) {
                        AddBlocker(&out,
                                   RepoBlockerKind::SubmoduleMappingMissing,
                                   "managed submodule path missing in .gitmodules: " + missingPath +
                                       " | repair: git config -f .gitmodules submodule.<name>.path '" + missingPath +
                                       "' && git config -f .gitmodules submodule.<name>.url <url> && git add .gitmodules");
                    } else {
                        AddUnique(&out.statusFlags, "UNREGISTERED_GITLINK_SKIPPED");
                        AddUnique(&out.diagnostics,
                                  "UNREGISTERED_GITLINK_SKIPPED: " + missingPath +
                                      " no .gitmodules mapping; not registered as managed submodule; skipped parent pointer update");
                    }
                } else {
                    AddBlocker(&out, RepoBlockerKind::KogPlanUnauditable,
                               "git submodule status --recursive failed: " + FirstLine(submoduleStatusOut.stderrStr));
                }
            }
        }

        const auto parsedStatuses = ParseSubmoduleStatusText(submoduleStatusText);
        const auto pathToName = ParseSubmodulePathToNameMap(InRepo);
        std::set<std::string> dirtySubmodulePaths;
        for (const auto& entry : parsedStatuses) {
            if (entry.marker == 'U') {
                AddBlocker(&out, RepoBlockerKind::SubmoduleStatusUnresolved,
                           "submodule status unresolved marker at " + entry.path);
            }
            if (entry.shaAllZero || entry.marker == 'U') {
                AddBlocker(&out, RepoBlockerKind::UnresolvedGitlink,
                           "unresolved gitlink pointer at " + entry.path + " sha=" + entry.sha);
            }
            if (entry.marker == '+' || entry.marker == '-') {
                dirtySubmodulePaths.insert(entry.path);
            }

            if (!InOptions.checkGitlinkReachability || entry.shaAllZero || entry.path.empty()) {
                continue;
            }

            // Only validate reachability for direct submodules declared in this repo's .gitmodules.
            // Recursive `git submodule status --recursive` also returns nested paths (e.g. a/b/c),
            // which belong to descendant repos and must be validated when scanning those repos.
            if (!pathToName.contains(entry.path)) {
                continue;
            }

            const auto childRepo = (InRepo / std::filesystem::path(entry.path)).lexically_normal();
            if (!std::filesystem::exists(childRepo)) {
                AddBlocker(&out, RepoBlockerKind::GitlinkUnreachable,
                           "submodule path missing locally for gitlink reachability: " + entry.path);
                continue;
            }

            const auto childTopOut = GitCapture(childRepo, {"rev-parse", "--show-toplevel"});
            if (childTopOut.exitCode != 0) {
                // Submodule path exists but is not initialized as a standalone repo yet.
                // Skip reachability checks here; they will be validated once initialized.
                continue;
            }
            if (RepoPathKey(Trim(childTopOut.stdoutStr)) != RepoPathKey(childRepo)) {
                // Git command resolved to an ancestor repository (common when nested submodule
                // worktree is not initialized). Treat as not initialized and skip.
                continue;
            }

            const auto catFileOut = GitCapture(childRepo, {"cat-file", "-e", entry.sha + "^{commit}"});
            if (catFileOut.exitCode != 0) {
                AddBlocker(&out, RepoBlockerKind::GitlinkUnreachable,
                           "gitlink commit missing locally after fetch: " + entry.path + " sha=" + entry.sha);
                continue;
            }

            const auto containsOut = GitCapture(childRepo, {"branch", "-r", "--contains", entry.sha});
            if (containsOut.exitCode != 0 || Trim(containsOut.stdoutStr).empty()) {
                std::string detail = "gitlink commit is not reachable from remote: " + entry.path + " sha=" + entry.sha;
                const auto expectedBranch = ExpectedSubmoduleBranch(InRepo, pathToName, entry.path);
                if (!expectedBranch.empty()) {
                    detail += " expectedBranch=" + expectedBranch;
                }
                AddBlocker(&out, RepoBlockerKind::GitlinkUnreachable, detail);
            }
        }

        out.hasDirtySubmodule = !dirtySubmodulePaths.empty();
        if (out.hasDirtySubmodule) {
            AddUnique(&out.statusFlags, "DIRTY_SUBMODULE");
            if (InOptions.blockOnDirtySubmodule) {
                AddBlocker(&out, RepoBlockerKind::DirtySubmodule,
                           "submodule worktree requires manual reconciliation: " + *dirtySubmodulePaths.begin());
            }
        }
    }

    std::sort(out.statusFlags.begin(), out.statusFlags.end());
    out.statusFlags.erase(std::unique(out.statusFlags.begin(), out.statusFlags.end()), out.statusFlags.end());
    std::sort(out.diagnostics.begin(), out.diagnostics.end());
    out.diagnostics.erase(std::unique(out.diagnostics.begin(), out.diagnostics.end()), out.diagnostics.end());
    return out;
}

} // namespace kano::git::workspace
