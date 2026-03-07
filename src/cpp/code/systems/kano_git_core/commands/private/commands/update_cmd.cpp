// update command — update discovered repos with native planner/waves

#include "command_registry.hpp"
#include "shell_executor.hpp"
#include "native_workspace.hpp"
#include "discovery.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

struct RepoUpdateResult {
    std::string path;
    std::string type;
    int exitCode = 0;
    bool skipped = false;
    std::string message;
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

auto RelativeDisplayPath(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::filesystem::path {
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

auto IsGitRepo(const std::filesystem::path& InRepo) -> bool {
    return GitCapture(InRepo, {"rev-parse", "--git-dir"}).exitCode == 0;
}

auto ResolveRepoFromSpec(const std::filesystem::path& InRoot,
                         const std::filesystem::path& InSpec,
                         const int InMaxDepth,
                         const bool InUseCache) -> std::filesystem::path {
    if (InSpec.empty() || InSpec == ".") {
        return InRoot.lexically_normal();
    }

    const auto specText = InSpec.generic_string();
    const auto candidate = (InSpec.is_absolute() ? InSpec : (InRoot / InSpec)).lexically_normal();
    if (std::filesystem::exists(candidate) && IsGitRepo(candidate)) {
        return candidate;
    }

    std::string manifestReason;
    if (const auto manifest = workspace::LoadTrustedWorkspaceManifest(InRoot, &manifestReason); manifest.has_value()) {
        std::vector<std::filesystem::path> exactMatches;
        std::vector<std::filesystem::path> fuzzyMatches;
        for (const auto& repo : manifest->repos) {
            const auto repoPath = repo.path.lexically_normal();
            const auto repoName = RepoNameFromPath(repoPath);
            const auto repoKey = repoPath.generic_string();
            const auto relativeKey = RelativeDisplayPath(InRoot, repoPath).generic_string();
            if (repoName == specText || repoKey == specText || relativeKey == specText) {
                exactMatches.push_back(repoPath);
                continue;
            }
            if (repoKey.find(specText) != std::string::npos || relativeKey.find(specText) != std::string::npos) {
                fuzzyMatches.push_back(repoPath);
            }
        }
        auto matches = exactMatches.empty() ? fuzzyMatches : exactMatches;
        std::sort(matches.begin(), matches.end(), [](const auto& A, const auto& B) {
            return A.generic_string() < B.generic_string();
        });
        matches.erase(std::unique(matches.begin(), matches.end(), [](const auto& A, const auto& B) {
            return A.generic_string() == B.generic_string();
        }), matches.end());
        if (matches.size() == 1) {
            return matches.front();
        }
        if (matches.size() > 1) {
            std::ostringstream oss;
            oss << "repo spec is ambiguous: " << specText << "\nMatches:\n";
            for (const auto& match : matches) {
                oss << "  - " << match.generic_string() << "\n";
            }
            throw std::runtime_error(oss.str());
        }
    }

    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = InMaxDepth;
    options.useCache = InUseCache;
    options.metadataLevel = "minimal";

    const auto discovery = workspace::DiscoverRepos(options);
    std::vector<std::filesystem::path> exactMatches;
    std::vector<std::filesystem::path> fuzzyMatches;
    for (const auto& repo : discovery.repos) {
        const auto repoPath = repo.path.lexically_normal();
        const auto repoName = RepoNameFromPath(repoPath);
        const auto repoKey = repoPath.generic_string();
        const auto relativeKey = RelativeDisplayPath(InRoot, repoPath).generic_string();
        if (repoName == specText || repoKey == specText || relativeKey == specText) {
            exactMatches.push_back(repoPath);
            continue;
        }
        if (repoKey.find(specText) != std::string::npos || relativeKey.find(specText) != std::string::npos) {
            fuzzyMatches.push_back(repoPath);
        }
    }

    auto matches = exactMatches.empty() ? fuzzyMatches : exactMatches;
    std::sort(matches.begin(), matches.end(), [](const auto& A, const auto& B) {
        return A.generic_string() < B.generic_string();
    });
    matches.erase(std::unique(matches.begin(), matches.end(), [](const auto& A, const auto& B) {
        return A.generic_string() == B.generic_string();
    }), matches.end());
    if (matches.empty()) {
        throw std::runtime_error("repo not found: " + specText);
    }
    if (matches.size() > 1) {
        std::ostringstream oss;
        oss << "repo spec is ambiguous: " << specText << "\nMatches:\n";
        for (const auto& match : matches) {
            oss << "  - " << match.generic_string() << "\n";
        }
        throw std::runtime_error(oss.str());
    }
    return matches.front();
}

auto DefaultBranchFromRemote(const std::filesystem::path& InRepo, const std::string& InRemote) -> std::string {
    const auto ref = GitCapture(InRepo, {"symbolic-ref", "--quiet", "--short", std::format("refs/remotes/{}/HEAD", InRemote)});
    if (ref.exitCode == 0) {
        const auto value = Trim(ref.stdoutStr);
        const auto slash = value.find('/');
        if (slash != std::string::npos && slash + 1 < value.size()) {
            return value.substr(slash + 1);
        }
    }

    const auto remoteShow = GitCapture(InRepo, {"remote", "show", InRemote});
    if (remoteShow.exitCode != 0) {
        return {};
    }
    std::istringstream iss(remoteShow.stdoutStr);
    std::string line;
    while (std::getline(iss, line)) {
        const std::string marker = "HEAD branch:";
        const auto pos = line.find(marker);
        if (pos == std::string::npos) {
            continue;
        }
        const auto branch = Trim(line.substr(pos + marker.size()));
        if (!branch.empty() && branch != "(unknown)") {
            return branch;
        }
    }
    return {};
}

auto UpdateRepoNative(const workspace::RepoRecord& InRepo, const std::string& InRemote, bool InDryRun) -> RepoUpdateResult {
    RepoUpdateResult out;
    out.path = InRepo.path.lexically_normal().generic_string();
    out.type = InRepo.type;

    const auto repoCheck = GitCapture(InRepo.path, {"rev-parse", "--git-dir"});
    if (repoCheck.exitCode != 0) {
        out.exitCode = 1;
        out.message = "Not a git repository";
        return out;
    }

    const auto remoteCheck = GitCapture(InRepo.path, {"remote", "get-url", InRemote});
    if (remoteCheck.exitCode != 0) {
        out.exitCode = 0;
        out.skipped = true;
        out.message = std::format("Skip: remote '{}' not found", InRemote);
        return out;
    }

    const auto currentBranchResult = GitCapture(InRepo.path, {"symbolic-ref", "--quiet", "--short", "HEAD"});
    const auto currentBranch = Trim(currentBranchResult.stdoutStr);
    if (currentBranchResult.exitCode != 0 || currentBranch.empty()) {
        out.exitCode = 0;
        out.skipped = true;
        out.message = "Skip: detached HEAD";
        return out;
    }

    std::string targetBranch = currentBranch;
    const auto remoteBranchCheck = GitCapture(InRepo.path, {"show-ref", "--verify", "--quiet", std::format("refs/remotes/{}/{}", InRemote, currentBranch)});
    if (remoteBranchCheck.exitCode != 0) {
        targetBranch = DefaultBranchFromRemote(InRepo.path, InRemote);
        if (targetBranch.empty()) {
            out.exitCode = 1;
            out.message = "Could not detect remote default branch";
            return out;
        }
    }

    if (InDryRun) {
        out.exitCode = 0;
        out.message = std::format("[DRY-RUN] Would fetch '{}' and rebase onto '{}/{}'", InRemote, InRemote, targetBranch);
        return out;
    }

    bool stashCreated = false;
    const auto changes = GitCapture(InRepo.path, {"status", "--porcelain"});
    if (changes.exitCode == 0 && !Trim(changes.stdoutStr).empty()) {
        const auto stashPush = GitCapture(InRepo.path, {"stash", "push", "-m", "auto-stash-workspace-update"});
        if (stashPush.exitCode == 0 && stashPush.stdoutStr.find("No local changes to save") == std::string::npos) {
            stashCreated = true;
        }
    }

    const auto fetch = GitCapture(InRepo.path, {"fetch", InRemote});
    if (fetch.exitCode != 0) {
        out.exitCode = 1;
        out.message = std::format("Fetch failed for remote '{}'", InRemote);
        return out;
    }

    const auto rebase = GitCapture(InRepo.path, {"rebase", std::format("{}/{}", InRemote, targetBranch)});
    if (rebase.exitCode != 0) {
        out.exitCode = 1;
        out.message = std::format("Rebase failed on '{}/{}'", InRemote, targetBranch);
        return out;
    }

    if (stashCreated) {
        const auto stashPop = GitCapture(InRepo.path, {"stash", "pop"});
        if (stashPop.exitCode != 0) {
            out.exitCode = 1;
            out.message = "Updated but failed to restore stash";
            return out;
        }
    }

    out.exitCode = 0;
    out.message = std::format("Updated via {}/{}", InRemote, targetBranch);
    return out;
}

void PrintRepoUpdateResult(const RepoUpdateResult& InResult) {
    std::cout << "\n==> [" << InResult.path << "] (" << InResult.type << ")\n";
    if (InResult.skipped) {
        std::cout << InResult.message << "\n";
        return;
    }
    if (InResult.exitCode == 0) {
        std::cout << InResult.message << "\n";
    } else {
        std::cerr << InResult.message << "\n";
    }
}

} // namespace

void RegisterUpdate(CLI::App& InApp) {
    auto* update = InApp.add_subcommand("update", "Update discovered repos with native planner/waves");
    update->allow_extras();
    auto* updateShell = new bool{false};
    auto* nativeUpdate = new bool{false};
    auto* nativePlan = new bool{false};
    auto* nativePlanOnly = new bool{false};
    auto* nativeMaxDepth = new int{3};
    auto* nativeNoCache = new bool{false};
    auto* nativeRefreshCache = new bool{false};
    auto* nativeNoIncremental = new bool{false};
    auto* nativeCacheTtl = new int{60};
    auto* nativeMaxStale = new int{900};
    auto* updateManifest = new std::string{};
    auto* updateIncludeTypes = new std::string{"root,registered,unregistered"};
    auto* updateExclude = new std::vector<std::string>{};
    auto* updateRemote = new std::string{"origin"};
    auto* updateMaxDepth = new int{3};
    auto* updateParallel = new int{1};
    auto* updateContinueOnError = new bool{false};
    auto* updateDryRun = new bool{false};
    auto* updateRepoPath = new std::string{};
    auto* updateRepoRoot = new std::string{};
    auto* updateTarget = new std::string{};

    update->add_flag("--native", *nativeUpdate, "Use native C++ wave executor for update operations (default)");
    update->add_flag("--shell", *updateShell, "Deprecated compatibility flag (shell path removed)");
    update->add_flag("--native-plan", *nativePlan, "Use native C++ discovery + scheduler plan");
    update->add_flag("--native-plan-only", *nativePlanOnly, "Emit native wave plan JSON only (no shell update execution)");
    update->add_option("--native-max-depth", *nativeMaxDepth, "Native discovery max depth");
    update->add_flag("--native-no-cache", *nativeNoCache, "Disable native discovery cache");
    update->add_flag("--native-refresh-cache", *nativeRefreshCache, "Force native cache refresh");
    update->add_flag("--native-no-incremental", *nativeNoIncremental, "Disable native incremental cache validation");
    update->add_option("--native-cache-ttl", *nativeCacheTtl, "Native cache TTL seconds");
    update->add_option("--native-max-stale", *nativeMaxStale, "Native incremental max stale seconds");
    update->add_option("--manifest", *updateManifest, "Use manifest file (default: auto-discover)");
    update->add_option("--include-types", *updateIncludeTypes, "Comma-separated: root,registered,unregistered");
    update->add_option("--exclude", *updateExclude, "Temporary scan-scope exclude override for this invocation only (repeatable; prefer .gitignore/.kogignore for shared policy)");
    update->add_option("--remote", *updateRemote, "Remote name (default: origin)");
    update->add_option("--max-depth", *updateMaxDepth, "Discovery max depth (default: 3)");
    update->add_option("--parallel", *updateParallel, "Parallel updates (default: 1)");
    update->add_flag("--continue-on-error", *updateContinueOnError, "Continue if a repo fails");
    update->add_flag("--dry-run", *updateDryRun, "Preview mode");
    update->add_option("--repo", *updateRepoPath, "Restrict update to a single repo path");
    update->add_option("--repo-root", *updateRepoRoot, "Workspace root/start path used for repo-name lookup");
    update->add_option("target", *updateTarget, "Optional repo target root (repo name or relative path)")->required(false);

    update->callback([=]() {
        if (*nativePlanOnly) {
            *nativePlan = true;
        }
        if (*updateShell) {
            std::cerr << "Error: --shell is no longer supported; update is fully native now\n";
            std::exit(2);
        }
        if (!*nativeUpdate && !*nativePlan) {
            *nativeUpdate = true;
        }
        auto extras = update->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native update mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }
        if (*nativePlan && !*nativePlanOnly) {
            *nativeUpdate = true;
            *nativePlan = false;
        }
        if (!*nativeUpdate && !*nativePlan) {
            std::cerr << "Error: update must run in native mode\n";
            std::exit(2);
        }
        if (*updateParallel <= 0) {
            std::cerr << "Error: --parallel must be >= 1\n";
            std::exit(1);
        }

        const auto invocationRoot = updateRepoRoot->empty() ? std::filesystem::current_path() : std::filesystem::path(*updateRepoRoot);
        const auto scopeRoot = updateTarget->empty()
            ? invocationRoot.lexically_normal()
            : ResolveRepoFromSpec(invocationRoot.lexically_normal(), std::filesystem::path(*updateTarget), 12, true);
        if (!updateTarget->empty() && !updateRepoPath->empty()) {
            std::cerr << "Error: positional target cannot be combined with --repo\n";
            std::exit(2);
        }

        workspace::DiscoverOptions options;
        options.rootDir = scopeRoot;
        options.maxDepth = *nativeMaxDepth;
        options.excludePatterns = *updateExclude;
        options.useCache = !*nativeNoCache;
        options.cacheTtlSeconds = *nativeCacheTtl;
        options.refreshCache = *nativeRefreshCache;
        options.incremental = !*nativeNoIncremental;
        options.maxStaleSeconds = *nativeMaxStale;
        options.metadataLevel = "full";

        const auto native = workspace::BuildNativeWorkspaceOutput(options, scopeRoot);
        if (*nativePlanOnly) {
            std::cout << native.updatePlanJson << "\n";
            std::exit(native.hasCycle ? 2 : 0);
        }
        if (native.hasCycle) {
            std::cerr << "Error: workspace dependency graph has cycle(s), aborting native plan execution.\n";
            std::cerr << native.wavesJson << "\n";
            std::exit(2);
        }

        std::vector<std::string> includeTypes;
        {
            std::stringstream ss(*updateIncludeTypes);
            std::string item;
            while (std::getline(ss, item, ',')) {
                if (item.empty()) {
                    continue;
                }
                if (item == "submodule") {
                    item = "registered";
                } else if (item == "standalone") {
                    item = "unregistered";
                }
                includeTypes.push_back(item);
            }
        }
        if (includeTypes.empty()) {
            includeTypes = {"root", "registered", "unregistered"};
        }

        const auto typeAllowed = [&includeTypes](const std::string& InType) {
            return std::find(includeTypes.begin(), includeTypes.end(), InType) != includeTypes.end();
        };
        const auto repoAllowed = [updateRepoPath, scopeRoot](const std::filesystem::path& InPath) {
            if (updateRepoPath->empty()) {
                return true;
            }
            const auto expected = ResolveRepoFromSpec(scopeRoot, std::filesystem::path(*updateRepoPath), 12, true).lexically_normal();
            return InPath.lexically_normal() == expected;
        };

        int successCount = 0;
        int failureCount = 0;
        int skippedCount = 0;

        for (const auto& wave : native.waves) {
            std::vector<workspace::RepoRecord> repos;
            repos.reserve(wave.size());
            for (const auto idx : wave) {
                if (idx >= native.discovery.repos.size()) {
                    continue;
                }
                const auto& repo = native.discovery.repos[idx];
                if (typeAllowed(repo.type) && repoAllowed(repo.path)) {
                    repos.push_back(repo);
                }
            }
            if (repos.empty()) {
                continue;
            }

            std::vector<RepoUpdateResult> results;
            if (*updateParallel <= 1 || repos.size() == 1) {
                for (const auto& repo : repos) {
                    results.push_back(UpdateRepoNative(repo, *updateRemote, *updateDryRun));
                }
            } else {
                std::vector<std::future<RepoUpdateResult>> futures;
                futures.reserve(repos.size());
                for (const auto& repo : repos) {
                    futures.push_back(std::async(std::launch::async, [repo, updateRemote, updateDryRun]() {
                        return UpdateRepoNative(repo, *updateRemote, *updateDryRun);
                    }));
                }
                for (auto& fut : futures) {
                    results.push_back(fut.get());
                }
            }

            for (const auto& result : results) {
                PrintRepoUpdateResult(result);
                if (result.exitCode == 0) {
                    successCount += 1;
                    if (result.skipped) {
                        skippedCount += 1;
                    }
                } else {
                    failureCount += 1;
                    if (!*updateContinueOnError) {
                        std::cerr << "Error: update failed, stopping (use --continue-on-error to continue)\n";
                        std::cout << "\nSummary: " << (successCount + failureCount) << " repos, "
                                  << successCount << " succeeded, " << failureCount << " failed"
                                  << " (" << skippedCount << " skipped)\n";
                        std::exit(1);
                    }
                }
            }
        }

        std::cout << "\nSummary: " << (successCount + failureCount) << " repos, "
                  << successCount << " succeeded, " << failureCount << " failed"
                  << " (" << skippedCount << " skipped)\n";
        std::exit(failureCount > 0 ? 1 : 0);
    });
}

} // namespace kano::git::commands
