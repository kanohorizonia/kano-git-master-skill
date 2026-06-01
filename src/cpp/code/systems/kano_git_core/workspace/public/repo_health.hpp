#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace kano::git::workspace {

enum class RepoBlockerKind {
    ActiveRebase,
    ActiveMerge,
    ActiveCherryPick,
    ActiveRevert,
    ActiveBisect,
    UnmergedPaths,
    UnresolvedGitlink,
    DetachedHead,
    DirtyWorktree,
    DirtySubmodule,
    FetchFailed,
    NoUpstream,
    BranchDiverged,
    UnpushedCommits,
    GitlinkUnreachable,
    SubmoduleStatusUnresolved,
    SubmoduleMappingMissing,
    KogPlanUnauditable,
};

struct RepoBlocker {
    RepoBlockerKind kind{};
    std::string reasonCode;
    std::string detail;
};

struct RepoHealthOptions {
    bool checkFetchRemotes = false;
    bool checkSubmoduleStatus = true;
    bool checkGitlinkReachability = true;
    bool fetchDryRun = true;
    std::string fetchRemoteOnly;
    bool blockOnDetachedHead = true;
    bool blockOnNoUpstream = false;
    bool blockOnUnpushedCommits = false;
    bool blockOnDirtyWorktree = false;
    bool blockOnDirtySubmodule = false;
    bool strictSubmoduleMappings = false;
    std::unordered_set<std::string> managedSubmodulePaths;
};

struct RepoHealth {
    std::string branch;
    std::string upstream;
    int ahead = 0;
    int behind = 0;
    bool detachedHead = false;
    bool hasUnmergedPaths = false;
    bool hasDirtyWorktree = false;
    bool hasDirtySubmodule = false;
    bool hasUnpushedCommits = false;
    std::vector<std::string> statusFlags;
    std::vector<std::string> diagnostics;
    std::vector<RepoBlocker> blockers;
};

struct ParsedSubmoduleStatusLine {
    bool valid = false;
    char marker = ' ';
    std::string sha;
    std::string path;
    bool shaAllZero = false;
};

auto RepoBlockerKindToReasonCode(RepoBlockerKind InKind) -> std::string;
auto ParseSubmoduleStatusLine(const std::string& InLine) -> ParsedSubmoduleStatusLine;
auto ScanRepoHealth(const std::filesystem::path& InRepo,
                    const RepoHealthOptions& InOptions = {}) -> RepoHealth;

} // namespace kano::git::workspace
