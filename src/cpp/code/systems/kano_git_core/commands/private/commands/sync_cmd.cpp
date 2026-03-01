// sync command — Repository synchronization workflows
// Delegates to: scripts/commit-tools/sync/smart-sync*.sh

#include "command_registry.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

struct SyncPlan {
    std::filesystem::path path;
    std::string type;
    std::string remote;
    std::string targetBranch;
    std::string branchSource;
};

enum class StableDevReportFormat {
    Compact,
    Table,
    Tsv,
    Json,
    Markdown,
};

struct StableDevSummaryRow {
    std::string repo;
    std::string result;
    std::string currentBranch;
    bool sameCommit{false};
    std::string latestUpstreamCommit;
    std::string latestStableCommit;
    std::string reason;
};

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

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto GitPassThrough(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::PassThrough, InRepo);
}

auto TruncateText(const std::string& InText, std::size_t InMax) -> std::string {
    if (InText.size() <= InMax) {
        return InText;
    }
    if (InMax <= 3) {
        return InText.substr(0, InMax);
    }
    return InText.substr(0, InMax - 3) + "...";
}

auto HasRemote(const std::filesystem::path& InRepo, const std::string& InRemote) -> bool {
    if (InRemote.empty()) {
        return false;
    }
    const auto result = GitCapture(InRepo, {"remote", "get-url", InRemote});
    return result.exitCode == 0;
}

auto ResolveRemote(const std::filesystem::path& InRepo, const std::string& InPreferredRemote) -> std::string {
    if (!InPreferredRemote.empty() && HasRemote(InRepo, InPreferredRemote)) {
        return InPreferredRemote;
    }
    if (HasRemote(InRepo, "origin")) {
        return "origin";
    }
    if (HasRemote(InRepo, "upstream")) {
        return "upstream";
    }
    const auto remotes = GitCapture(InRepo, {"remote"});
    if (remotes.exitCode != 0) {
        return {};
    }
    std::istringstream iss(remotes.stdoutStr);
    std::string line;
    if (std::getline(iss, line)) {
        return Trim(line);
    }
    return {};
}

auto DetectRemoteDefaultBranch(const std::filesystem::path& InRepo, const std::string& InRemote) -> std::string {
    const auto remoteHead = GitCapture(InRepo, {"symbolic-ref", "--quiet", std::format("refs/remotes/{}/HEAD", InRemote)});
    if (remoteHead.exitCode == 0) {
        const auto ref = Trim(remoteHead.stdoutStr);
        const auto marker = std::format("refs/remotes/{}/", InRemote);
        if (ref.starts_with(marker) && ref.size() > marker.size()) {
            return ref.substr(marker.size());
        }
    }

    for (const std::string branch : {"main", "master", "dev", "develop", "trunk"}) {
        const auto exists = GitCapture(InRepo, {"show-ref", "--verify", "--quiet", std::format("refs/remotes/{}/{}", InRemote, branch)});
        if (exists.exitCode == 0) {
            return branch;
        }
    }
    return {};
}

auto CurrentBranch(const std::filesystem::path& InRepo) -> std::string {
    const auto result = GitCapture(InRepo, {"symbolic-ref", "--quiet", "--short", "HEAD"});
    if (result.exitCode != 0) {
        return {};
    }
    return Trim(result.stdoutStr);
}

auto ParseStableDevReportFormat(const std::string& InValue) -> std::optional<StableDevReportFormat> {
    if (InValue == "compact") {
        return StableDevReportFormat::Compact;
    }
    if (InValue == "table") {
        return StableDevReportFormat::Table;
    }
    if (InValue == "tsv") {
        return StableDevReportFormat::Tsv;
    }
    if (InValue == "json") {
        return StableDevReportFormat::Json;
    }
    if (InValue == "markdown") {
        return StableDevReportFormat::Markdown;
    }
    return std::nullopt;
}

auto IsAgentModeEnabled() -> bool {
    const char* value = std::getenv("KANO_AGENT_MODE");
    if (value == nullptr) {
        return false;
    }
    const std::string normalized = Trim(value);
    return normalized == "1" || normalized == "true" || normalized == "TRUE";
}

auto HasLongFlag(const std::vector<std::string>& InArgs, const std::string& InFlag) -> bool {
    for (const auto& arg : InArgs) {
        if (arg == InFlag || arg.starts_with(InFlag + "=")) {
            return true;
        }
    }
    return false;
}

auto JsonEscape(const std::string& InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size() + 16);
    for (const char c : InValue) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

auto ResolveLatestStableTag(const std::filesystem::path& InRepo) -> std::string {
    static const std::regex stableTagPattern(R"((release[-_/])?(v)?[0-9]+(\.[0-9]+){1,3}(\+[0-9A-Za-z.-]+)?)", std::regex::icase);
    const auto tags = GitCapture(InRepo, {"tag", "--list", "--sort=-version:refname"});
    if (tags.exitCode != 0) {
        return {};
    }

    std::istringstream iss(tags.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        if (std::regex_match(line, stableTagPattern)) {
            return line;
        }
    }
    return {};
}

auto ResolveCommitSha(const std::filesystem::path& InRepo, const std::string& InRev) -> std::string {
    if (InRev.empty()) {
        return {};
    }
    const auto result = GitCapture(InRepo, {"rev-parse", InRev});
    if (result.exitCode != 0) {
        return {};
    }
    return Trim(result.stdoutStr);
}

auto ResolveCommitLine(const std::filesystem::path& InRepo, const std::string& InRev) -> std::string {
    if (InRev.empty()) {
        return "N/A";
    }
    const auto result = GitCapture(InRepo, {"show", "-s", "--format=%h | %cI | %an | %s", InRev});
    if (result.exitCode != 0) {
        return "N/A";
    }
    const auto line = Trim(result.stdoutStr);
    return line.empty() ? "N/A" : line;
}

auto ResolveGitmodulesBranch(const std::filesystem::path& InRoot, const std::string& InRelPath) -> std::string {
    const auto paths = GitCapture(InRoot, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
    if (paths.exitCode != 0) {
        return {};
    }

    std::istringstream iss(paths.stdoutStr);
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
        const auto key = line.substr(0, sp);
        const auto value = line.substr(sp + 1);
        if (value != InRelPath || !key.ends_with(".path")) {
            continue;
        }
        const auto prefix = key.substr(0, key.size() - 5);
        const auto branch = GitCapture(InRoot, {"config", "-f", ".gitmodules", "--get", prefix + ".branch"});
        if (branch.exitCode == 0) {
            return Trim(branch.stdoutStr);
        }
        return {};
    }

    return {};
}

auto ResolveStableRef(const std::filesystem::path& InRoot, const std::filesystem::path& InRepo, const std::string& InCurrentBranch, const std::string& InRelPath) -> std::string {
    if (InCurrentBranch.starts_with("branch_")) {
        return InCurrentBranch;
    }

    const auto gmBranch = ResolveGitmodulesBranch(InRoot, InRelPath);
    if (!gmBranch.empty()) {
        if (GitCapture(InRepo, {"show-ref", "--verify", "--quiet", "refs/heads/" + gmBranch}).exitCode == 0) {
            return gmBranch;
        }
        if (GitCapture(InRepo, {"show-ref", "--verify", "--quiet", "refs/remotes/origin/" + gmBranch}).exitCode == 0) {
            return "origin/" + gmBranch;
        }
    }

    return InCurrentBranch;
}

auto CollectSrcSubmodulePaths(const std::filesystem::path& InRoot) -> std::vector<std::string> {
    std::vector<std::string> out;
    const auto paths = GitCapture(InRoot, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
    if (paths.exitCode != 0) {
        return out;
    }

    std::istringstream iss(paths.stdoutStr);
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
        const auto rel = line.substr(sp + 1);
        if (rel.starts_with("src/")) {
            out.push_back(rel);
        }
    }
    return out;
}

void PrintStableDevSummary(const std::vector<StableDevSummaryRow>& InRows, StableDevReportFormat InFormat) {
    switch (InFormat) {
        case StableDevReportFormat::Compact: {
            for (const auto& row : InRows) {
                std::cout << "[" << row.repo << "] RESULT=" << row.result << " | branch=" << row.currentBranch << "\n";
                if (!row.reason.empty()) {
                    std::cout << "  reason: " << row.reason << "\n";
                }
                if (row.sameCommit) {
                    std::cout << "  commit: " << row.latestUpstreamCommit << "\n";
                } else {
                    std::cout << "  upstream: " << row.latestUpstreamCommit << "\n";
                    std::cout << "  stable:   " << row.latestStableCommit << "\n";
                }
            }
            break;
        }
        case StableDevReportFormat::Table: {
            const std::string sep = "+------------------------------+---------+------------------------+------------------------------------------------------------+------------------------------------------------------------+------------------------------+";
            std::cout << sep << "\n";
            std::cout << std::format("| {:<28} | {:<7} | {:<22} | {:<58} | {:<58} | {:<28} |\n",
                "Repo", "Result", "Current Branch", "Latest Upstream Commit (sha|time|author|title)", "Latest Stable Commit (sha|time|author|title)", "Reason");
            std::cout << sep << "\n";
            for (const auto& row : InRows) {
                std::cout << std::format("| {:<28} | {:<7} | {:<22} | {:<58} | {:<58} | {:<28} |\n",
                    TruncateText(row.repo, 28),
                    TruncateText(row.result, 7),
                    TruncateText(row.currentBranch, 22),
                    TruncateText(row.latestUpstreamCommit, 58),
                    TruncateText(row.latestStableCommit, 58),
                    TruncateText(row.reason, 28));
            }
            std::cout << sep << "\n";
            break;
        }
        case StableDevReportFormat::Tsv: {
            std::cout << "repo\tresult\tcurrent_branch\tsame_commit\tlatest_upstream_commit\tlatest_stable_commit\treason\n";
            for (const auto& row : InRows) {
                std::cout << row.repo << "\t" << row.result << "\t" << row.currentBranch << "\t"
                          << (row.sameCommit ? "1" : "0") << "\t" << row.latestUpstreamCommit << "\t"
                          << row.latestStableCommit << "\t" << row.reason << "\n";
            }
            break;
        }
        case StableDevReportFormat::Json: {
            std::cout << "[\n";
            for (std::size_t i = 0; i < InRows.size(); ++i) {
                const auto& row = InRows[i];
                std::cout << std::format(
                    "  {{\"repo\":\"{}\",\"result\":\"{}\",\"current_branch\":\"{}\",\"same_commit\":{},\"latest_upstream_commit\":\"{}\",\"latest_stable_commit\":\"{}\",\"reason\":\"{}\"}}{}\n",
                    JsonEscape(row.repo),
                    JsonEscape(row.result),
                    JsonEscape(row.currentBranch),
                    row.sameCommit ? "true" : "false",
                    JsonEscape(row.latestUpstreamCommit),
                    JsonEscape(row.latestStableCommit),
                    JsonEscape(row.reason),
                    (i + 1 < InRows.size()) ? "," : "");
            }
            std::cout << "]\n";
            break;
        }
        case StableDevReportFormat::Markdown: {
            std::cout << "| Repo | Result | Current Branch | Latest Upstream Commit | Latest Stable Commit | Reason |\n";
            std::cout << "| --- | --- | --- | --- | --- | --- |\n";
            for (const auto& row : InRows) {
                auto esc = [](std::string value) {
                    std::size_t pos = 0;
                    while ((pos = value.find('|', pos)) != std::string::npos) {
                        value.replace(pos, 1, "\\|");
                        pos += 2;
                    }
                    return value;
                };
                std::cout << "| " << row.repo << " | " << row.result << " | " << row.currentBranch << " | "
                          << esc(row.latestUpstreamCommit) << " | " << esc(row.latestStableCommit) << " | " << esc(row.reason) << " |\n";
            }
            break;
        }
    }
}

auto RunStableDevWorkspace(
    const std::filesystem::path& InRoot,
    StableDevReportFormat InFormat,
    std::vector<std::string> InForwardArgs) -> int {
    if (HasLongFlag(InForwardArgs, "--repo")) {
        std::cerr << "ERROR: --repo is not allowed with --workspace\n";
        return 1;
    }

    if (IsAgentModeEnabled() && !HasLongFlag(InForwardArgs, "--ai-resolve") && !HasLongFlag(InForwardArgs, "--no-ai-resolve")) {
        InForwardArgs.push_back("--no-ai-resolve");
    }

    const auto gitmodules = InRoot / ".gitmodules";
    if (!std::filesystem::exists(gitmodules)) {
        std::cerr << "ERROR: .gitmodules not found at project root: " << gitmodules.generic_string() << "\n";
        return 1;
    }

    const auto submodulePaths = CollectSrcSubmodulePaths(InRoot);
    if (submodulePaths.empty()) {
        std::cout << "INFO: No src/* submodules found. Nothing to do.\n";
        return 0;
    }

    int success = 0;
    int skipped = 0;
    int failed = 0;
    std::vector<StableDevSummaryRow> summary;
    summary.reserve(submodulePaths.size());

    for (const auto& rel : submodulePaths) {
        const auto repoPath = InRoot / rel;
        if (GitCapture(repoPath, {"rev-parse", "--is-inside-work-tree"}).exitCode != 0) {
            std::cout << "SKIP: " << rel << " (not initialized git repo)\n";
            skipped += 1;
            summary.push_back({rel, "SKIPPED", "N/A", false, "N/A", "N/A", "not initialized git repo"});
            continue;
        }

        if (!HasRemote(repoPath, "upstream")) {
            std::cout << "SKIP: " << rel << " (no upstream remote)\n";
            skipped += 1;
            summary.push_back({rel, "SKIPPED", CurrentBranch(repoPath), false, "N/A", "N/A", "no upstream remote"});
            continue;
        }

        std::cout << "RUN: " << rel << "\n";
        std::vector<std::string> args = {"--repo", repoPath.generic_string()};
        args.insert(args.end(), InForwardArgs.begin(), InForwardArgs.end());
        const auto run = shell::ExecuteScript("commit-tools/sync/smart-sync-stable-dev.sh", args);

        const auto branch = CurrentBranch(repoPath);
        const auto latestTag = ResolveLatestStableTag(repoPath);
        const auto upstreamSha = latestTag.empty() ? std::string{} : ResolveCommitSha(repoPath, std::format("refs/tags/{}^{{commit}}", latestTag));
        const auto stableRef = ResolveStableRef(InRoot, repoPath, branch, rel);
        const auto stableSha = ResolveCommitSha(repoPath, stableRef);
        const auto upstreamLine = ResolveCommitLine(repoPath, upstreamSha);
        const auto stableLine = ResolveCommitLine(repoPath, stableSha);

        if (run.exitCode == 0) {
            success += 1;
            const bool same = !upstreamSha.empty() && !stableSha.empty() && upstreamSha == stableSha;
            summary.push_back({
                rel,
                "SUCCESS",
                branch,
                same,
                upstreamLine,
                same ? "(same as upstream)" : stableLine,
                ""
            });
        } else {
            failed += 1;
            std::cerr << "FAIL: " << rel << "\n";
            summary.push_back({
                rel,
                "FAILED",
                branch,
                false,
                upstreamLine,
                stableLine,
                "workflow failed"
            });
        }
    }

    std::cout << "=== upstream-stable-dev wrapper summary ===\n";
    std::cout << "success: " << success << "\n";
    std::cout << "skipped: " << skipped << "\n";
    std::cout << "failed: " << failed << "\n";
    std::cout << "OVERALL RESULT: " << ((failed > 0) ? "FAILED" : "SUCCESS") << "\n";
    std::cout << "=== upstream-stable-dev branch report ===\n";
    PrintStableDevSummary(summary, InFormat);

    return failed > 0 ? 1 : 0;
}

auto DiscoverRegisteredPathsRecursive(const std::filesystem::path& InWorkspaceRoot) -> std::set<std::string> {
    std::set<std::string> out;
    std::vector<std::filesystem::path> queue{InWorkspaceRoot};

    while (!queue.empty()) {
        const auto current = queue.back();
        queue.pop_back();

        const auto gitmodules = current / ".gitmodules";
        if (!std::filesystem::exists(gitmodules)) {
            continue;
        }

        const auto pathsResult = GitCapture(current, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
        if (pathsResult.exitCode != 0) {
            continue;
        }

        std::istringstream iss(pathsResult.stdoutStr);
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
            const auto relPath = line.substr(sp + 1);
            const auto full = std::filesystem::weakly_canonical(current / relPath).generic_string();
            if (out.insert(full).second) {
                queue.push_back(full);
            }
        }
    }

    return out;
}

auto GitmodulesBranchForPath(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InRepoPath) -> std::optional<std::string> {
    auto current = InRepoPath.parent_path();
    const auto workspace = std::filesystem::weakly_canonical(InWorkspaceRoot);
    const auto target = std::filesystem::weakly_canonical(InRepoPath);

    while (!current.empty()) {
        const auto candidateGitmodules = current / ".gitmodules";
        if (std::filesystem::exists(candidateGitmodules)) {
            std::error_code ec;
            auto rel = std::filesystem::relative(target, current, ec);
            if (!ec) {
                const auto relPath = rel.generic_string();
                const auto pathResult = GitCapture(current, {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"});
                if (pathResult.exitCode == 0) {
                    std::istringstream iss(pathResult.stdoutStr);
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
                        const auto key = line.substr(0, sp);
                        const auto value = line.substr(sp + 1);
                        if (value != relPath) {
                            continue;
                        }

                        const std::string suffix = ".path";
                        if (!key.ends_with(suffix)) {
                            continue;
                        }
                        const auto prefix = key.substr(0, key.size() - suffix.size());
                        const auto branchResult = GitCapture(current, {"config", "-f", ".gitmodules", "--get", prefix + ".branch"});
                        if (branchResult.exitCode == 0) {
                            const auto branch = Trim(branchResult.stdoutStr);
                            if (!branch.empty()) {
                                return branch;
                            }
                        }
                        return std::nullopt;
                    }
                }
            }
        }

        if (std::filesystem::weakly_canonical(current) == workspace) {
            break;
        }
        const auto next = current.parent_path();
        if (next == current) {
            break;
        }
        current = next;
    }

    return std::nullopt;
}

auto BuildSyncPlans(
    const std::filesystem::path& InRoot,
    const std::string& InPreferredRemote,
    int InMaxDepth,
    bool InNoCache,
    bool InRefreshCache) -> std::pair<std::vector<SyncPlan>, std::string> {
    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = InMaxDepth;
    options.useCache = !InNoCache;
    options.refreshCache = InRefreshCache;
    options.metadataLevel = "full";

    const auto discovery = workspace::DiscoverRepos(options);
    const auto root = std::filesystem::weakly_canonical(InRoot);
    const auto registeredPaths = DiscoverRegisteredPathsRecursive(root);

    std::vector<SyncPlan> plans;
    plans.reserve(discovery.repos.size());

    for (const auto& repo : discovery.repos) {
        const auto repoPath = std::filesystem::weakly_canonical(repo.path);
        const auto remote = ResolveRemote(repoPath, InPreferredRemote);
        if (remote.empty()) {
            std::cerr << "WARN: Skip repo without remotes: " << repoPath.generic_string() << "\n";
            continue;
        }

        const auto current = CurrentBranch(repoPath);
        const bool isRoot = (repoPath == root);
        const bool isRegistered = registeredPaths.contains(repoPath.generic_string());

        std::string branchSource;
        std::string targetBranch;
        if (isRoot) {
            if (!current.empty()) {
                targetBranch = current;
                branchSource = "root current branch";
            } else {
                targetBranch = DetectRemoteDefaultBranch(repoPath, remote);
                branchSource = "root detached -> remote default";
            }
        } else if (isRegistered) {
            const auto configured = GitmodulesBranchForPath(root, repoPath);
            if (configured.has_value() && !configured->empty()) {
                targetBranch = *configured;
                branchSource = "registered .gitmodules branch";
            } else {
                targetBranch = DetectRemoteDefaultBranch(repoPath, remote);
                branchSource = "registered remote default branch";
            }
        } else {
            if (!current.empty()) {
                targetBranch = current;
                branchSource = "unregistered current branch";
            } else {
                targetBranch = DetectRemoteDefaultBranch(repoPath, remote);
                branchSource = "unregistered detached -> remote default";
            }
        }

        if (targetBranch.empty()) {
            std::cerr << "ERROR: Could not determine target branch for repo: " << repoPath.generic_string() << "\n";
            continue;
        }

        plans.push_back(SyncPlan{
            .path = repoPath,
            .type = isRoot ? "root" : (isRegistered ? "registered" : "unregistered"),
            .remote = remote,
            .targetBranch = targetBranch,
            .branchSource = branchSource,
        });
    }

    std::sort(plans.begin(), plans.end(), [](const SyncPlan& A, const SyncPlan& B) {
        return A.path.generic_string() < B.path.generic_string();
    });

    return {plans, discovery.mode};
}

auto RunNativeOriginLatestSync(
    const std::filesystem::path& InRepoRoot,
    const std::string& InRemote,
    int InMaxDepth,
    bool InDryRun,
    bool InNoCache,
    bool InRefreshCache) -> int {
    const auto [plans, mode] = BuildSyncPlans(InRepoRoot, InRemote, InMaxDepth, InNoCache, InRefreshCache);
    std::cout << "Syncing workspace repos with recursive branch rules\n";
    std::cout << "Discover mode: " << (mode.empty() ? "unknown" : mode) << "\n";

    int failures = 0;
    for (const auto& plan : plans) {
        const auto rel = std::filesystem::relative(plan.path, InRepoRoot).generic_string();
        const auto name = (rel.empty() || rel == ".") ? "." : rel;

        std::cout << (InDryRun ? "[DRY RUN] " : "") << "Repo: " << name << "\n";
        std::cout << (InDryRun ? "[DRY RUN] " : "") << "Branch source: " << plan.branchSource << "\n";

        const auto fetch = GitCapture(plan.path, {"fetch", plan.remote, "--prune", "--tags"});
        if (!InDryRun && fetch.exitCode != 0) {
            std::cerr << "WARN: fetch failed for " << name << "\n";
        }

        const auto hasLocal = GitCapture(plan.path, {"show-ref", "--verify", "--quiet", std::format("refs/heads/{}", plan.targetBranch)}).exitCode == 0;
        const auto hasRemote = GitCapture(plan.path, {"show-ref", "--verify", "--quiet", std::format("refs/remotes/{}/{}", plan.remote, plan.targetBranch)}).exitCode == 0;

        std::vector<std::string> checkoutArgs;
        if (hasLocal) {
            checkoutArgs = {"checkout", plan.targetBranch};
        } else if (hasRemote) {
            checkoutArgs = {"checkout", "-b", plan.targetBranch, std::format("{}/{}", plan.remote, plan.targetBranch)};
        } else {
            if (plan.type == "unregistered") {
                checkoutArgs = {"checkout", plan.targetBranch};
                std::cout << "WARN: Unregistered repo branch has no remote ref, keeping local branch: " << name << "\n";
            } else {
                std::cerr << "ERROR: Target branch not found for " << name << "\n";
                failures += 1;
                continue;
            }
        }

        if (InDryRun) {
            std::cout << "[DRY RUN] Would run: git";
            for (const auto& arg : checkoutArgs) {
                std::cout << " " << arg;
            }
            std::cout << "\n";
            if (hasRemote) {
                std::cout << "[DRY RUN] Would run: git pull --rebase " << plan.remote << " " << plan.targetBranch << "\n";
            } else {
                std::cout << "[DRY RUN] Skip pull: missing remote branch " << plan.remote << "/" << plan.targetBranch << "\n";
            }
            continue;
        }

        const auto checkout = GitPassThrough(plan.path, checkoutArgs);
        if (checkout.exitCode != 0) {
            std::cerr << "ERROR: checkout failed for " << name << "\n";
            failures += 1;
            continue;
        }

        if (hasRemote) {
            const auto pull = GitPassThrough(plan.path, {"pull", "--rebase", plan.remote, plan.targetBranch});
            if (pull.exitCode != 0) {
                std::cerr << "WARN: pull --rebase failed for " << name << "\n";
            }
        }
    }

    std::cout << "=== Sync Complete ===\n";
    return failures > 0 ? 1 : 0;
}

} // namespace

void RegisterSync(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("sync", "Repository synchronization workflows");

    // --- sync origin-latest ---
    auto* origin_latest = cmd->add_subcommand("origin-latest", "Sync to origin default branch latest");
    origin_latest->allow_extras();
    auto* originLatestShell = new bool{false};
    auto* originLatestRepo = new std::string{"."};
    auto* originLatestRemote = new std::string{"origin"};
    auto* originLatestDryRun = new bool{false};
    auto* originLatestMaxDepth = new int{6};
    auto* originLatestNoCache = new bool{false};
    auto* originLatestRefreshCache = new bool{false};

    origin_latest->add_flag("--shell", *originLatestShell, "Use shell fallback implementation");
    origin_latest->add_option("--repo", *originLatestRepo, "Target repository root path");
    origin_latest->add_option("--remote", *originLatestRemote, "Preferred remote name");
    origin_latest->add_flag("--dry-run", *originLatestDryRun, "Preview sync actions without modifying repositories");
    origin_latest->add_option("--native-max-depth", *originLatestMaxDepth, "Native discovery max depth");
    origin_latest->add_flag("--native-no-cache", *originLatestNoCache, "Disable native discovery cache");
    origin_latest->add_flag("--native-refresh-cache", *originLatestRefreshCache, "Force native cache refresh");

    origin_latest->callback([=]() {
        auto extras = origin_latest->remaining();
        if (*originLatestShell || !extras.empty()) {
            std::vector<std::string> args;
            if (!originLatestRepo->empty() && *originLatestRepo != ".") {
                args.push_back("--repo");
                args.push_back(*originLatestRepo);
            }
            if (!originLatestRemote->empty()) {
                args.push_back("--remote");
                args.push_back(*originLatestRemote);
            }
            if (*originLatestDryRun) {
                args.push_back("--dry-run");
            }
            args.insert(args.end(), extras.begin(), extras.end());
            auto result = shell::ExecuteScript("commit-tools/sync/smart-sync-origin-latest.sh", args);
            std::exit(result.exitCode);
        }

        const auto repoRoot = std::filesystem::weakly_canonical(std::filesystem::path(*originLatestRepo));
        const auto code = RunNativeOriginLatestSync(
            repoRoot,
            *originLatestRemote,
            *originLatestMaxDepth,
            *originLatestDryRun,
            *originLatestNoCache,
            *originLatestRefreshCache);
        std::exit(code);
    });

    // --- sync upstream-force-push ---
    auto* upstream_fp = cmd->add_subcommand("upstream-force-push", "Sync from upstream, force-push to origin");
    upstream_fp->allow_extras();
    upstream_fp->callback([=]() {
        auto extras = upstream_fp->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("commit-tools/sync/smart-sync-upstream-force-push.sh", args);
        std::exit(result.exitCode);
    });

    // --- sync stable-dev ---
    auto* stable_dev = cmd->add_subcommand("stable-dev", "Stable-dev sync (tag-based cherry-pick migration)");
    stable_dev->allow_extras();
    auto* stableDevWorkspace = new bool{false};
    auto* stableDevReportFormat = new std::string{"compact"};
    stable_dev->add_flag("--workspace", *stableDevWorkspace, "Run stable-dev across src/* submodules with aggregated summary report");
    stable_dev->add_option("--format", *stableDevReportFormat, "Workspace report format: compact|table|tsv|json|markdown");
    stable_dev->callback([=]() {
        auto extras = stable_dev->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());

        if (*stableDevWorkspace) {
            const auto format = ParseStableDevReportFormat(*stableDevReportFormat);
            if (!format.has_value()) {
                std::cerr << "ERROR: Unsupported --format: " << *stableDevReportFormat << " (supported: compact, table, tsv, json, markdown)\n";
                std::exit(1);
            }

            std::filesystem::path workspaceRoot;
            if (const char* rootEnv = std::getenv("KANO_GIT_MASTER_ROOT"); rootEnv != nullptr && std::string(rootEnv).size() > 0) {
                workspaceRoot = std::filesystem::weakly_canonical(std::filesystem::path(rootEnv));
            } else {
                workspaceRoot = std::filesystem::current_path();
            }
            std::exit(RunStableDevWorkspace(workspaceRoot, *format, args));
        }

        auto result = shell::ExecuteScript("commit-tools/sync/smart-sync-stable-dev.sh", args);
        std::exit(result.exitCode);
    });

    // --- sync dev ---
    auto* dev = cmd->add_subcommand("dev", "Dev sync (upstream default branch tip)");
    dev->allow_extras();
    dev->callback([=]() {
        auto extras = dev->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("commit-tools/sync/smart-sync-dev.sh", args);
        std::exit(result.exitCode);
    });

    // --- sync (default: auto-detect) ---
    cmd->allow_extras();
    cmd->callback([=]() {
        if (cmd->get_subcommands().empty()) {
            auto extras = cmd->remaining();
            std::vector<std::string> args(extras.begin(), extras.end());
            auto result = shell::ExecuteScript("commit-tools/sync/smart-sync.sh", args);
            std::exit(result.exitCode);
        }
    });
}

} // namespace kano::git::commands
