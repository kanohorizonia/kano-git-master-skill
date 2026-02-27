// sync command — Repository synchronization workflows
// Delegates to: scripts/commit-tools/sync/smart-sync*.sh

#include "command_registry.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <iostream>
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
    stable_dev->callback([=]() {
        auto extras = stable_dev->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
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
