#include "commit_push_post_sync.hpp"

#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace kano::git::commands {
namespace {

struct PostSyncChangedPathSet {
    std::vector<std::string> paths;
    bool hasUntracked = false;
};

auto Trim(std::string InValue) -> std::string {
    while (!InValue.empty() &&
           (InValue.back() == '\n' || InValue.back() == '\r' || InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() && (InValue[start] == ' ' || InValue[start] == '\t')) {
        start += 1;
    }
    return InValue.substr(start);
}

auto ToLower(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return InValue;
}

auto TrimTrailingWindowsPathChars(std::string InValue) -> std::string {
    while (!InValue.empty() && (InValue.back() == ' ' || InValue.back() == '.')) {
        InValue.pop_back();
    }
    return InValue;
}

auto IsWindowsReservedDeviceComponent(std::string InComponent) -> bool {
#if defined(_WIN32)
    InComponent = TrimTrailingWindowsPathChars(ToLower(Trim(std::move(InComponent))));
    if (InComponent.empty() || InComponent == "." || InComponent == "..") {
        return false;
    }

    const auto colon = InComponent.find(':');
    if (colon != std::string::npos) {
        InComponent = InComponent.substr(0, colon);
    }

    const auto dot = InComponent.find('.');
    const auto stem = dot == std::string::npos ? InComponent : InComponent.substr(0, dot);
    static const std::unordered_set<std::string> reserved = {
        "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"
    };
    return reserved.contains(stem);
#else
    (void)InComponent;
    return false;
#endif
}

auto PathHasWindowsReservedDeviceComponent(const std::string& InPath) -> bool {
#if defined(_WIN32)
    if (InPath.empty()) {
        return false;
    }

    std::string normalized = InPath;
    for (auto& ch : normalized) {
        if (ch == '\\') {
            ch = '/';
        }
    }

    std::size_t start = 0;
    while (start <= normalized.size()) {
        const auto end = normalized.find('/', start);
        auto component = end == std::string::npos ? normalized.substr(start) : normalized.substr(start, end - start);
        if (IsWindowsReservedDeviceComponent(component)) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return false;
#else
    (void)InPath;
    return false;
#endif
}

auto ParseNonNegativeInt(const std::string& InValue) -> int {
    const auto trimmed = Trim(InValue);
    if (trimmed.empty()) {
        return 0;
    }
    try {
        return std::max(0, std::stoi(trimmed));
    } catch (const std::exception&) {
        return 0;
    }
}

auto RelativeDisplayPath(const std::filesystem::path& InRoot,
                         const std::filesystem::path& InPath) -> std::filesystem::path {
    auto normalizedRoot = InRoot.lexically_normal();
    if (!normalizedRoot.is_absolute()) {
        normalizedRoot = std::filesystem::absolute(normalizedRoot).lexically_normal();
    }
    const auto normalizedPath = InPath.lexically_normal();
    const auto relative = normalizedPath.lexically_relative(normalizedRoot);
    if (!relative.empty()) {
        return relative;
    }
    return normalizedPath;
}

auto IsParentPath(const std::filesystem::path& InParent, const std::filesystem::path& InChild) -> bool {
    const auto parent = InParent.lexically_normal().generic_string();
    const auto child = InChild.lexically_normal().generic_string();
    if (parent.empty() || child.empty() || parent == child) {
        return false;
    }
    const std::string prefix = parent + "/";
    return child.rfind(prefix, 0) == 0;
}

auto RepoNameFromPath(const std::filesystem::path& InPath) -> std::string {
    const auto normalized = InPath.lexically_normal();
    auto name = normalized.filename().generic_string();
    if (name.empty()) {
        name = normalized.parent_path().filename().generic_string();
    }
    if (!name.empty()) {
        return name;
    }
    return normalized.generic_string();
}

auto PostSyncPathAllowedByScope(const PostSyncPlanPathScope* InScope,
                                const std::filesystem::path& InRepoRoot,
                                const std::string& InPath) -> bool {
    if (InScope == nullptr || !InScope->scoped) {
        return true;
    }

    const auto repoIt = InScope->repos.find(CommitPushRepoScopeKey(InRepoRoot));
    if (repoIt == InScope->repos.end()) {
        return false;
    }

    const auto path = NormalizeCommitPushGitPath(InPath);
    bool included = false;
    for (const auto& include : repoIt->second.include) {
        if (CommitPushPathspecCoversPath(include, path)) {
            included = true;
            break;
        }
    }
    if (!included) {
        return false;
    }

    for (const auto& exclude : repoIt->second.exclude) {
        if (CommitPushPathspecCoversPath(exclude, path)) {
            return false;
        }
    }
    return true;
}

auto FilterIgnoredReservedStatusLines(const std::string& InStatusPorcelain) -> std::vector<std::string> {
    std::vector<std::string> kept;
    std::istringstream iss(InStatusPorcelain);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.size() < 4) {
            continue;
        }
        auto path = Trim(line.substr(3));
        const auto renamePos = path.find(" -> ");
        if (renamePos != std::string::npos) {
            path = Trim(path.substr(renamePos + 4));
        }
        if (path.empty()) {
            continue;
        }
        if (PathHasWindowsReservedDeviceComponent(path)) {
            continue;
        }
        kept.push_back(line);
    }
    return kept;
}

auto ParsePostSyncChangedPaths(const std::string& InStatusPorcelain,
                               const std::filesystem::path& InRepoRoot,
                               const PostSyncPlanPathScope* InScope) -> PostSyncChangedPathSet {
    PostSyncChangedPathSet out;
    for (const auto& line : FilterIgnoredReservedStatusLines(InStatusPorcelain)) {
        if (line.size() < 4) {
            continue;
        }
        std::string path = Trim(line.substr(3));
        if (path.empty()) {
            continue;
        }
        const auto renamePos = path.find(" -> ");
        if (renamePos != std::string::npos) {
            path = Trim(path.substr(renamePos + 4));
        }
        if (path.empty() || !PostSyncPathAllowedByScope(InScope, InRepoRoot, path)) {
            continue;
        }
        if (line.rfind("?? ", 0) == 0) {
            out.hasUntracked = true;
            continue;
        }
        out.paths.push_back(path);
    }
    return out;
}

auto IsGitlinkPathInHead(const std::filesystem::path& InRepoRoot, const std::string& InPath) -> bool {
    const auto tree = shell::ExecuteCommand("git", {"ls-tree", "HEAD", "--", InPath}, shell::ExecMode::Capture, InRepoRoot);
    if (tree.exitCode != 0) {
        return false;
    }
    const auto out = Trim(tree.stdoutStr);
    if (out.empty()) {
        return false;
    }
    return out.rfind("160000 ", 0) == 0;
}

auto IsRegisteredSubmodulePath(const std::filesystem::path& InRepoRoot, const std::string& InPath) -> bool {
    const auto normalizedPath = Trim(InPath);
    if (normalizedPath.empty()) {
        return false;
    }

    const auto config = shell::ExecuteCommand(
        "git",
        {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"},
        shell::ExecMode::Capture,
        InRepoRoot);
    if (config.exitCode != 0) {
        return false;
    }

    std::istringstream iss(config.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        const auto sp = line.find(' ');
        if (sp == std::string::npos || sp + 1 >= line.size()) {
            continue;
        }
        const auto submodulePath = Trim(line.substr(sp + 1));
        if (!submodulePath.empty() && submodulePath == normalizedPath) {
            return true;
        }
    }
    return false;
}

auto CollectGitlinkOnlyChangedPaths(const std::filesystem::path& InRepoRoot,
                                    const PostSyncPlanPathScope* InScope) -> std::vector<std::string> {
    const auto status = shell::ExecuteCommand("git", {"status", "--porcelain"}, shell::ExecMode::Capture, InRepoRoot);
    if (status.exitCode != 0) {
        return {};
    }
    const auto& porcelain = status.stdoutStr;
    if (Trim(porcelain).empty()) {
        return {};
    }
    const auto changed = ParsePostSyncChangedPaths(porcelain, InRepoRoot, InScope);
    if (changed.hasUntracked || changed.paths.empty()) {
        return {};
    }
    for (const auto& path : changed.paths) {
        if (!IsRegisteredSubmodulePath(InRepoRoot, path)) {
            return {};
        }
        if (!IsGitlinkPathInHead(InRepoRoot, path)) {
            return {};
        }
    }
    return changed.paths;
}

auto BuildNestedRepoWaves(const std::vector<std::filesystem::path>& InRepos)
    -> std::vector<std::vector<std::filesystem::path>> {
    const std::size_t count = InRepos.size();
    if (count <= 1) {
        return {InRepos};
    }

    std::vector<int> indegree(count, 0);
    std::vector<std::vector<std::size_t>> reverseEdges(count);
    for (std::size_t parent = 0; parent < count; ++parent) {
        for (std::size_t child = 0; child < count; ++child) {
            if (parent == child) {
                continue;
            }
            if (IsParentPath(InRepos[parent], InRepos[child])) {
                indegree[parent] += 1;
                reverseEdges[child].push_back(parent);
            }
        }
    }

    std::vector<std::size_t> frontier;
    frontier.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (indegree[i] == 0) {
            frontier.push_back(i);
        }
    }

    std::vector<std::vector<std::filesystem::path>> waves;
    std::size_t processed = 0;
    while (!frontier.empty()) {
        std::sort(frontier.begin(), frontier.end(), [&](const std::size_t A, const std::size_t B) {
            return InRepos[A].lexically_normal().generic_string() < InRepos[B].lexically_normal().generic_string();
        });

        std::vector<std::filesystem::path> wave;
        std::vector<std::size_t> next;
        for (const auto idx : frontier) {
            wave.push_back(InRepos[idx]);
            processed += 1;
            for (const auto dependent : reverseEdges[idx]) {
                indegree[dependent] -= 1;
                if (indegree[dependent] == 0) {
                    next.push_back(dependent);
                }
            }
        }
        waves.push_back(std::move(wave));
        frontier = std::move(next);
    }

    if (processed != count) {
        return {InRepos};
    }
    return waves;
}

auto RepoCurrentBranch(const std::filesystem::path& InRepoRoot) -> std::string {
    const auto branch = shell::ExecuteCommand(
        "git", {"symbolic-ref", "--quiet", "--short", "HEAD"}, shell::ExecMode::Capture, InRepoRoot);
    if (branch.exitCode != 0) {
        return {};
    }
    return Trim(branch.stdoutStr);
}

auto RepoHasRemote(const std::filesystem::path& InRepoRoot, const std::string& InRemote) -> bool {
    return shell::ExecuteCommand("git", {"remote", "get-url", InRemote}, shell::ExecMode::Capture, InRepoRoot).exitCode == 0;
}

auto RepoPushRemotes(const std::filesystem::path& InRepoRoot) -> std::vector<std::string> {
    std::vector<std::string> remotes;
    if (RepoHasRemote(InRepoRoot, "origin-ssh")) {
        remotes.push_back("origin-ssh");
    }
    if (RepoHasRemote(InRepoRoot, "origin-http")) {
        remotes.push_back("origin-http");
    }
    if (RepoHasRemote(InRepoRoot, "origin")) {
        remotes.push_back("origin");
    }
    return remotes;
}

auto RepoHasCommitsToPushToRemote(const std::filesystem::path& InRepoRoot,
                                  const std::string& InRemote,
                                  const std::string& InBranch) -> bool {
    if (InRemote.empty() || InBranch.empty()) {
        return false;
    }
    const auto remoteRef = std::format("refs/remotes/{}/{}", InRemote, InBranch);
    const auto localRef = std::format("refs/heads/{}", InBranch);
    const auto hasRemoteRef =
        shell::ExecuteCommand("git", {"show-ref", "--verify", "--quiet", remoteRef}, shell::ExecMode::Capture, InRepoRoot).exitCode == 0;
    if (!hasRemoteRef) {
        return true;
    }
    const auto ahead = shell::ExecuteCommand(
        "git", {"rev-list", "--count", std::format("{}..{}", remoteRef, localRef)}, shell::ExecMode::Capture, InRepoRoot);
    if (ahead.exitCode != 0) {
        return true;
    }
    return ParseNonNegativeInt(ahead.stdoutStr) > 0;
}

auto RepoHeadIsUnpublishedAcrossPushRemotes(const std::filesystem::path& InRepoRoot) -> bool {
    const auto branch = RepoCurrentBranch(InRepoRoot);
    if (branch.empty()) {
        return false;
    }
    const auto remotes = RepoPushRemotes(InRepoRoot);
    if (remotes.empty()) {
        return false;
    }
    for (const auto& remote : remotes) {
        if (!RepoHasCommitsToPushToRemote(InRepoRoot, remote, branch)) {
            return false;
        }
    }
    return true;
}

auto BuildGitlinkOnlyFollowupCommitMessage(const std::filesystem::path& InWorkspaceRoot,
                                           const std::filesystem::path& InRepoRoot) -> std::string {
    const auto relative = RelativeDisplayPath(InWorkspaceRoot, InRepoRoot).generic_string();
    if (relative.empty() || relative == ".") {
        return "chore(workspace): update submodule pointers";
    }
    return std::format("chore({}): update submodule pointers", RepoNameFromPath(InRepoRoot));
}

} // namespace

auto NormalizeCommitPushGitPath(std::string InPath) -> std::string {
    std::replace(InPath.begin(), InPath.end(), '\\', '/');
    while (InPath.rfind("./", 0) == 0) {
        InPath = InPath.substr(2);
    }
    return Trim(InPath);
}

auto CommitPushPathspecCoversPath(std::string InPathspec, std::string InPath) -> bool {
    InPathspec = NormalizeCommitPushGitPath(std::move(InPathspec));
    InPath = NormalizeCommitPushGitPath(std::move(InPath));
    if (InPathspec.empty() || InPath.empty()) {
        return false;
    }
    if (InPathspec == InPath) {
        return true;
    }
    if (InPathspec.back() != '/') {
        InPathspec.push_back('/');
    }
    return InPath.rfind(InPathspec, 0) == 0;
}

auto CommitPushRepoScopeKey(const std::filesystem::path& InRepoRoot) -> std::string {
    return InRepoRoot.lexically_normal().generic_string();
}

auto RepoHasPostSyncWorkingTreeChanges(const std::filesystem::path& InRepoRoot) -> bool {
    const auto status = shell::ExecuteCommand("git", {"status", "--porcelain"}, shell::ExecMode::Capture, InRepoRoot);
    return status.exitCode == 0 && !FilterIgnoredReservedStatusLines(status.stdoutStr).empty();
}

auto AnyRepoHasPostSyncWorkingTreeChanges(const std::vector<std::filesystem::path>& InRepos,
                                          const PostSyncPlanPathScope* InScope) -> bool {
    if (InScope == nullptr || !InScope->scoped) {
        for (const auto& repo : InRepos) {
            const auto status = shell::ExecuteCommand("git", {"status", "--porcelain"}, shell::ExecMode::Capture, repo);
            if (status.exitCode == 0 && !Trim(status.stdoutStr).empty()) {
                return true;
            }
        }
        return false;
    }

    for (const auto& repo : InRepos) {
        const auto status = shell::ExecuteCommand("git", {"status", "--porcelain"}, shell::ExecMode::Capture, repo);
        if (status.exitCode != 0) {
            continue;
        }
        const auto changed = ParsePostSyncChangedPaths(status.stdoutStr, repo, InScope);
        if (changed.hasUntracked || !changed.paths.empty()) {
            return true;
        }
    }
    return false;
}

auto ClassifyPostSyncDelta(const std::vector<std::filesystem::path>& InCandidateRepos,
                           const PostSyncPlanPathScope* InScope) -> PostSyncDeltaSummary {
    PostSyncDeltaSummary summary;
    for (const auto& repo : InCandidateRepos) {
        const auto status = shell::ExecuteCommand("git", {"status", "--porcelain"}, shell::ExecMode::Capture, repo);
        if (status.exitCode != 0) {
            continue;
        }
        const auto& porcelain = status.stdoutStr;
        if (Trim(porcelain).empty()) {
            continue;
        }
        if (FilterIgnoredReservedStatusLines(porcelain).empty()) {
            continue;
        }
        const auto changed = ParsePostSyncChangedPaths(porcelain, repo, InScope);
        if (!changed.hasUntracked && changed.paths.empty()) {
            continue;
        }
        const auto gitlinkPaths = CollectGitlinkOnlyChangedPaths(repo, InScope);
        if (!gitlinkPaths.empty()) {
            summary.gitlinkOnlyRepoCount += 1;
            continue;
        }
        summary.semanticRepos.push_back(repo);
    }
    if (!summary.semanticRepos.empty()) {
        summary.kind = PostSyncDeltaKind::SemanticDrift;
    } else if (summary.gitlinkOnlyRepoCount > 0) {
        summary.kind = PostSyncDeltaKind::GitlinkOnly;
    }
    return summary;
}

auto AutoAmendGitlinkOnlyPostSyncRepos(const std::filesystem::path& InWorkspaceRoot,
                                       const std::vector<std::filesystem::path>& InCandidateRepos,
                                       const PostSyncPlanPathScope* InScope) -> std::pair<int, int> {
    int updatedCount = 0;
    const auto maxPasses = std::max<std::size_t>(InCandidateRepos.size(), 1);
    for (std::size_t pass = 0; pass < maxPasses; ++pass) {
        bool changedThisPass = false;
        const auto repoWaves = BuildNestedRepoWaves(InCandidateRepos);
        for (const auto& wave : repoWaves) {
            for (const auto& repo : wave) {
                const auto gitlinkPaths = CollectGitlinkOnlyChangedPaths(repo, InScope);
                if (gitlinkPaths.empty()) {
                    continue;
                }
                std::vector<std::string> addArgs = {"add", "--"};
                addArgs.insert(addArgs.end(), gitlinkPaths.begin(), gitlinkPaths.end());
                const auto addResult = shell::ExecuteCommand("git", addArgs, shell::ExecMode::PassThrough, repo);
                if (addResult.exitCode != 0) {
                    std::cerr << "[commit-push] post-sync gitlink-only auto-amend failed: git add -- <gitlink paths> failed in "
                              << repo.generic_string() << "\n";
                    return {-1, updatedCount};
                }

                if (RepoHeadIsUnpublishedAcrossPushRemotes(repo)) {
                    const auto amendResult = shell::ExecuteCommand(
                        "git", {"commit", "--amend", "--no-edit"}, shell::ExecMode::PassThrough, repo);
                    if (amendResult.exitCode != 0) {
                        std::cerr << "[commit-push] post-sync gitlink-only auto-amend failed: git commit --amend --no-edit failed in "
                                  << repo.generic_string() << "\n";
                        return {-1, updatedCount};
                    }
                } else {
                    const auto commitResult = shell::ExecuteCommand(
                        "git",
                        {"commit", "-m", BuildGitlinkOnlyFollowupCommitMessage(InWorkspaceRoot, repo)},
                        shell::ExecMode::PassThrough,
                        repo);
                    if (commitResult.exitCode != 0) {
                        std::cerr << "[commit-push] post-sync gitlink-only follow-up commit failed in "
                                  << repo.generic_string() << "\n";
                        return {-1, updatedCount};
                    }
                }
                updatedCount += 1;
                changedThisPass = true;
            }
        }
        if (!changedThisPass) {
            return {0, updatedCount};
        }
    }

    for (const auto& repo : InCandidateRepos) {
        if (!CollectGitlinkOnlyChangedPaths(repo, InScope).empty()) {
            std::cerr << "[commit-push] post-sync gitlink-only convergence failed; repo still dirty after iterative passes: "
                      << repo.generic_string() << "\n";
            return {-1, updatedCount};
        }
    }
    return {0, updatedCount};
}

} // namespace kano::git::commands
