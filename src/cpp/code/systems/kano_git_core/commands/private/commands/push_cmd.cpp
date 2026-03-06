// push command — Multi-remote push workflow
// Delegates to: scripts/commit-tools/smart-push.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"
#include "discovery.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <vector>

namespace kano::git::commands {
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

auto ToLower(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return InValue;
}

auto LooksLikeLfsPushFailure(const shell::ExecResult& InResult) -> bool {
    const auto merged = ToLower(InResult.stdoutStr + "\n" + InResult.stderrStr);
    if (merged.find("git-lfs-authenticate") != std::string::npos) {
        return true;
    }
    if (merged.find("batch request") != std::string::npos && merged.find("lfs") != std::string::npos) {
        return true;
    }
    if (merged.find("uploading lfs objects") != std::string::npos &&
        merged.find("failed to push some refs") != std::string::npos) {
        return true;
    }
    return false;
}

auto PrintCapturedOutputIfAny(const shell::ExecResult& InResult) -> void {
    if (!InResult.stdoutStr.empty()) {
        std::cout << InResult.stdoutStr;
    }
    if (!InResult.stderrStr.empty()) {
        std::cerr << InResult.stderrStr;
    }
}

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto GitPassThrough(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::PassThrough, InRepo);
}

auto HasRemote(const std::filesystem::path& InRepo, const std::string& InRemote) -> bool {
    const auto out = GitCapture(InRepo, {"remote", "get-url", InRemote});
    return out.exitCode == 0;
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

auto HasCommitsToPush(const std::filesystem::path& InRepo,
                      const std::string& InRemote,
                      const std::string& InBranch) -> bool {
    const auto remoteRef = std::format("refs/remotes/{}/{}", InRemote, InBranch);
    const auto localRef = std::format("refs/heads/{}", InBranch);

    const auto hasRemoteRef = GitCapture(InRepo, {"show-ref", "--verify", "--quiet", remoteRef}).exitCode == 0;
    if (!hasRemoteRef) {
        return true;
    }

    const auto ahead = GitCapture(InRepo, {"rev-list", "--count", std::format("{}..{}", remoteRef, localRef)});
    if (ahead.exitCode != 0) {
        return true;
    }

    return ParseNonNegativeInt(ahead.stdoutStr) > 0;
}

auto ParseReposCsv(const std::string& InCsv) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> out;
    std::istringstream iss(InCsv);
    std::string token;
    while (std::getline(iss, token, ',')) {
        const auto trimmed = Trim(token);
        if (trimmed.empty()) {
            continue;
        }
        out.emplace_back(trimmed);
    }
    return out;
}

auto IsInternalOperationalRepoPath(const std::filesystem::path& InRoot, const std::filesystem::path& InRepo) -> bool {
    auto rel = InRepo.lexically_relative(InRoot).generic_string();
    std::replace(rel.begin(), rel.end(), '\\', '/');
    const auto lower = ToLower(rel);
    return lower == ".kano" || lower.rfind(".kano/", 0) == 0 || lower.find("/.kano/") != std::string::npos ||
           lower == "src/cpp/build" || lower.rfind("src/cpp/build/", 0) == 0 || lower.find("/src/cpp/build/") != std::string::npos;
}

auto DiscoverWorkspaceRepos(const std::filesystem::path& InRoot) -> std::vector<std::filesystem::path> {
    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = 12;
    // Push must operate on the live workspace repo graph so newly added/nested
    // repos are not silently skipped by a stale discovery cache.
    options.useCache = false;
    options.metadataLevel = "minimal";
    // Push discovery should enumerate the live workspace repo graph without
    // folder-name heuristics. Push/skip decisions belong to explicit policy
    // checks in the execution stage, not discovery.
    options.excludePatterns.clear();

    const auto discovery = workspace::DiscoverRepos(options);
    std::vector<std::filesystem::path> repos;
    repos.reserve(discovery.repos.size());
    for (const auto& repo : discovery.repos) {
        const auto path = repo.path.lexically_normal();
        if (IsInternalOperationalRepoPath(InRoot, path)) {
            continue;
        }
        repos.push_back(path);
    }

    std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return A.generic_string() < B.generic_string();
    });
    repos.erase(std::unique(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return A.generic_string() == B.generic_string();
    }), repos.end());
    return repos;
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

auto GroupFromRelativePath(const std::filesystem::path& InRelativePath) -> std::string {
    const auto parent = InRelativePath.parent_path().generic_string();
    if (parent.empty() || parent == ".") {
        return ".";
    }
    return parent;
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

auto ResolveGitmodulesPushPolicy(const std::filesystem::path& InRepo) -> std::string {
    auto repo = std::filesystem::weakly_canonical(InRepo).lexically_normal();
    auto current = repo.parent_path();

    while (!current.empty()) {
        const auto gitmodulesPath = current / ".gitmodules";
        if (std::filesystem::exists(gitmodulesPath)) {
            const auto relative = repo.lexically_relative(current);
            const auto rel = relative.generic_string();
            if (!rel.empty() && rel != "." && !rel.starts_with("..")) {
                const auto key = std::format("submodule.{}.kog-push-policy", rel);
                const auto configured = GitCapture(current, {"config", "-f", ".gitmodules", "--get", key});
                if (configured.exitCode == 0) {
                    const auto policy = ToLower(Trim(configured.stdoutStr));
                    if (!policy.empty()) {
                        return policy;
                    }
                }
            }
        }

        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
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

auto BuildPushWaves(const std::vector<std::filesystem::path>& InRepos) -> std::vector<std::vector<std::filesystem::path>> {
    const std::size_t n = InRepos.size();
    if (n <= 1) {
        return {InRepos};
    }

    std::vector<int> indegree(n, 0);
    std::vector<std::vector<std::size_t>> reverseEdges(n);

    for (std::size_t parent = 0; parent < n; ++parent) {
        for (std::size_t child = 0; child < n; ++child) {
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
    frontier.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
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
        wave.reserve(frontier.size());
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

    if (processed != n) {
        return {InRepos};
    }
    return waves;
}

auto DetectDefaultPushJobs() -> int {
    const unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) {
        return 1;
    }
    return static_cast<int>(cores);
}

auto RunNativePush(
    const std::vector<std::filesystem::path>& InRepos,
    const bool InSkipSync,
    const bool InFetchOnly,
    const bool InDryRun,
    const bool InForceWithLease,
    const bool InNoVerify,
    const bool InStashLocalChanges,
    const bool InFailOnDirtySync,
    const int InJobs,
    const bool InProfile,
    const bool InVerbose,
    const std::string& InRemoteFilter) -> int {
    const auto totalStart = std::chrono::steady_clock::now();
    long long syncMillis = 0;
    long long pushMillis = 0;
    int maxParallelObserved = 1;
    std::mutex statsMutex;
    std::vector<std::tuple<std::string, std::string, std::string>> pushStats;
    int failures = 0;
    int successes = 0;
    std::mutex outputMutex;

    std::unordered_map<std::string, std::size_t> repoOrderByPath;
    repoOrderByPath.reserve(InRepos.size());
    for (std::size_t i = 0; i < InRepos.size(); ++i) {
        const auto repoPath = std::filesystem::weakly_canonical(InRepos[i]).lexically_normal().generic_string();
        repoOrderByPath[repoPath] = i + 1;
    }

    auto runOneRepo = [&](const std::filesystem::path& repoPathRaw) -> std::pair<int, int> {
        const auto repoPath = std::filesystem::weakly_canonical(repoPathRaw);
        const auto repoLabel = repoPath.lexically_normal().generic_string();
        std::size_t repoIndex = 0;
        if (const auto it = repoOrderByPath.find(repoLabel); it != repoOrderByPath.end()) {
            repoIndex = it->second;
        }

        {
            std::lock_guard<std::mutex> lock(outputMutex);
            if (repoIndex > 0) {
                std::cout << "[" << repoIndex << "/" << InRepos.size() << "] Processing " << repoLabel << "\n";
            } else {
                std::cout << "[?/?] Processing " << repoLabel << "\n";
            }
        }

        const auto gitDir = GitCapture(repoPath, {"rev-parse", "--git-dir"});
        if (gitDir.exitCode != 0) {
            std::cerr << "Error: Not a git repository: " << repoLabel << "\n";
            return {0, 1};
        }

        if (InFetchOnly) {
            if (InDryRun) {
                std::cout << "[DRY RUN] [" << repoLabel << "] Would run: git fetch --all --prune --tags\n";
            } else {
                const auto fetch = GitPassThrough(repoPath, {"fetch", "--all", "--prune", "--tags"});
                if (fetch.exitCode != 0) {
                    std::cerr << "[" << repoLabel << "] Fetch failed\n";
                    return {0, 1};
                }
            }
            std::cout << "[" << repoLabel << "] Fetch-only mode: skipping rebase and push\n";
            return {1, 0};
        }

        const auto pushPolicy = ResolveGitmodulesPushPolicy(repoPath);
        if (pushPolicy == "skip") {
            std::cout << "[" << repoLabel << "] Push skipped by .gitmodules policy (kog-push-policy=skip)\n";
            return {1, 0};
        }
        if (!pushPolicy.empty() && pushPolicy != "allow") {
            std::cerr << "[" << repoLabel << "] Warning: unknown .gitmodules kog-push-policy='" << pushPolicy
                      << "'; expected skip|allow, treating as allow\n";
        }

        const auto branchOut = GitCapture(repoPath, {"symbolic-ref", "--quiet", "--short", "HEAD"});
        const auto branch = Trim(branchOut.stdoutStr);
        if (branchOut.exitCode != 0 || branch.empty()) {
            std::cerr << "Error: Detached HEAD is not supported by native push flow: " << repoLabel << "\n";
            return {0, 1};
        }

        const bool hasUpstream = (GitCapture(repoPath, {"rev-parse", "--abbrev-ref", "@{upstream}"}).exitCode == 0);
        const bool hasLocalChanges = !Trim(GitCapture(repoPath, {"status", "--porcelain"}).stdoutStr).empty();

        if (!InSkipSync && hasUpstream) {
            const auto syncStart = std::chrono::steady_clock::now();
            bool hadStash = false;
            std::string stashName;
            if (hasLocalChanges) {
                if (InFailOnDirtySync) {
                    std::cerr << "[" << repoLabel << "] Sync failed: local changes present (--fail-on-dirty-sync)\n";
                    return {0, 1};
                }

                if (InStashLocalChanges) {
                    stashName = "kano-native-push-autostash";
                    if (InDryRun) {
                        std::cout << "[DRY RUN] [" << repoLabel << "] Would run: git stash push -u -m " << stashName << "\n";
                        hadStash = true;
                    } else {
                        const auto stash = GitCapture(repoPath, {"stash", "push", "-u", "-m", stashName});
                        if (stash.exitCode != 0) {
                            std::cerr << "[" << repoLabel << "] Failed to create auto-stash before sync\n";
                            return {0, 1};
                        }
                        const auto stashOut = Trim(stash.stdoutStr);
                        hadStash = stashOut.find("No local changes to save") == std::string::npos;
                        if (hadStash) {
                            std::cout << "[" << repoLabel << "] Auto-stashed local changes for sync\n";
                        }
                    }
                } else {
                    std::cout << "[" << repoLabel << "] Sync skipped: local changes present; proceeding to push\n";
                }
            } else {
                const auto upstreamRef = Trim(GitCapture(repoPath, {"rev-parse", "--abbrev-ref", "@{upstream}"}).stdoutStr);
                bool shouldPullRebase = true;
                int aheadCount = -1;
                int behindCount = -1;
                if (!upstreamRef.empty()) {
                    const auto aheadBehind = GitCapture(repoPath, {"rev-list", "--left-right", "--count", std::format("HEAD...{}", upstreamRef)});
                    if (aheadBehind.exitCode == 0) {
                        std::istringstream iss(aheadBehind.stdoutStr);
                        if (iss >> aheadCount >> behindCount) {
                            shouldPullRebase = behindCount > 0;
                        }
                    }
                }

                if (InDryRun) {
                    if (shouldPullRebase) {
                        std::cout << "[DRY RUN] [" << repoLabel << "] Would run: git pull --rebase\n";
                    } else {
                        std::cout << "[DRY RUN] [" << repoLabel << "] Skip sync pull: local branch is not behind upstream";
                        if (aheadCount >= 0 && behindCount >= 0) {
                            std::cout << " (ahead=" << aheadCount << ", behind=" << behindCount << ")";
                        }
                        std::cout << "\n";
                    }
                } else if (shouldPullRebase) {
                    const auto pull = GitPassThrough(repoPath, {"pull", "--rebase"});
                    if (pull.exitCode != 0) {
                        std::cerr << "[" << repoLabel << "] Sync failed before push\n";
                        return {0, 1};
                    }
                } else {
                    std::cout << "[" << repoLabel << "] Sync skipped: local branch is not behind upstream";
                    if (aheadCount >= 0 && behindCount >= 0) {
                        std::cout << " (ahead=" << aheadCount << ", behind=" << behindCount << ")";
                    }
                    std::cout << "\n";
                }
            }

            if (InStashLocalChanges && hasLocalChanges && hadStash) {
                if (InDryRun) {
                    std::cout << "[DRY RUN] [" << repoLabel << "] Would run: git stash pop\n";
                } else {
                    const auto pop = GitPassThrough(repoPath, {"stash", "pop"});
                    if (pop.exitCode != 0) {
                        std::cerr << "[" << repoLabel << "] Failed to restore auto-stash after sync\n";
                        return {0, 1};
                    }
                }
            }
            const auto syncEnd = std::chrono::steady_clock::now();
            syncMillis += std::chrono::duration_cast<std::chrono::milliseconds>(syncEnd - syncStart).count();
        }

        std::vector<std::string> pushRemotes;
        if (!InRemoteFilter.empty()) {
            if (HasRemote(repoPath, InRemoteFilter)) {
                pushRemotes.push_back(InRemoteFilter);
            }
        } else {
            if (HasRemote(repoPath, "origin-ssh")) {
                pushRemotes.push_back("origin-ssh");
            }
            if (HasRemote(repoPath, "origin-http")) {
                pushRemotes.push_back("origin-http");
            }
            if (HasRemote(repoPath, "origin")) {
                pushRemotes.push_back("origin");
            }
        }

        if (pushRemotes.empty()) {
            std::cerr << "Error: No pushable origin remote found for " << repoLabel << "\n";
            return {0, 1};
        }

        std::vector<std::string> pushArgs;
        if (InForceWithLease) {
            pushArgs.push_back("--force-with-lease");
        }
        if (InNoVerify) {
            pushArgs.push_back("--no-verify");
        }
        if (!hasUpstream) {
            pushArgs.push_back("-u");
        }

        int repoSuccess = 0;
        const auto pushStart = std::chrono::steady_clock::now();
        for (const auto& remote : pushRemotes) {
            const bool hasSomethingToPush = HasCommitsToPush(repoPath, remote, branch);
            if (!hasSomethingToPush) {
                if (InVerbose) {
                    std::cout << "[" << repoLabel << "] Unchanged (" << remote << ")\n";
                }
                repoSuccess = 1;
                continue;
            }

            std::vector<std::string> args = {"push"};
            args.insert(args.end(), pushArgs.begin(), pushArgs.end());
            args.push_back(remote);
            args.push_back(branch);

            if (InDryRun) {
                std::cout << "[DRY RUN] [" << repoLabel << "] Would run: git";
                for (const auto& arg : args) {
                    std::cout << " " << arg;
                }
                std::cout << "\n";
                repoSuccess = 1;
                {
                    std::lock_guard<std::mutex> lock(statsMutex);
                    pushStats.emplace_back(repoLabel, remote, branch);
                }
                continue;
            }

            auto result = GitCapture(repoPath, args);
            if (InVerbose) {
                PrintCapturedOutputIfAny(result);
            }
            if (result.exitCode == 0) {
                std::cout << "[" << repoLabel << "] Pushed (" << remote << ")\n";
                repoSuccess = 1;
                {
                    std::lock_guard<std::mutex> lock(statsMutex);
                    pushStats.emplace_back(repoLabel, remote, branch);
                }
            } else {
                bool recoveredByLfsRetry = false;
                if (!InDryRun && LooksLikeLfsPushFailure(result)) {
                    std::cerr << "[" << repoLabel << "] Push failed (" << remote
                              << ") due to LFS transport/auth issue; attempting git lfs push retry\n";

                    const auto lfsPush = GitCapture(repoPath, {"lfs", "push", remote, branch});
                    if (InVerbose || lfsPush.exitCode != 0) {
                        PrintCapturedOutputIfAny(lfsPush);
                    }

                    if (lfsPush.exitCode == 0) {
                        auto retryResult = GitCapture(repoPath, args);
                        if (InVerbose || retryResult.exitCode != 0) {
                            PrintCapturedOutputIfAny(retryResult);
                        }
                        if (retryResult.exitCode == 0) {
                            std::cout << "[" << repoLabel << "] Pushed (" << remote
                                      << ") after LFS retry\n";
                            repoSuccess = 1;
                            recoveredByLfsRetry = true;
                            std::lock_guard<std::mutex> lock(statsMutex);
                            pushStats.emplace_back(repoLabel, remote, branch);
                        }
                    }
                }

                if (recoveredByLfsRetry) {
                    continue;
                }

                if (InVerbose) {
                    std::cerr << "[" << repoLabel << "] Push failed (" << remote << ")\n";
                }
            }
        }

        if (repoSuccess == 0) {
            return {0, 1};
        }
        const auto pushEnd = std::chrono::steady_clock::now();
        pushMillis += std::chrono::duration_cast<std::chrono::milliseconds>(pushEnd - pushStart).count();
        return {1, 0};
    };

    const int jobs = InJobs < 1 ? 1 : InJobs;
    const auto waves = BuildPushWaves(InRepos);
    for (const auto& wave : waves) {
        if (jobs == 1 || wave.size() <= 1) {
            for (const auto& repoPathRaw : wave) {
                const auto [s, f] = runOneRepo(repoPathRaw);
                successes += s;
                failures += f;
            }
            continue;
        }

        std::vector<std::future<std::pair<int, int>>> active;
        active.reserve(static_cast<std::size_t>(jobs));

        auto waitOne = [&]() {
            if (active.empty()) {
                return;
            }
            auto result = active.front().get();
            successes += result.first;
            failures += result.second;
            active.erase(active.begin());
        };

        for (const auto& repoPathRaw : wave) {
            while (static_cast<int>(active.size()) >= jobs) {
                waitOne();
            }
            active.push_back(std::async(std::launch::async, [&, repoPathRaw]() {
                return runOneRepo(repoPathRaw);
            }));
            if (static_cast<int>(active.size()) > maxParallelObserved) {
                maxParallelObserved = static_cast<int>(active.size());
            }
        }

        while (!active.empty()) {
            waitOne();
        }
    }

    if (!pushStats.empty()) {
        const auto root = std::filesystem::current_path().lexically_normal();
        std::map<std::string, std::vector<std::tuple<std::string, std::string, std::string>>> grouped;

        for (const auto& stat : pushStats) {
            const std::filesystem::path repoPath(std::get<0>(stat));
            const auto relative = RelativeDisplayPath(root, repoPath);
            const auto group = GroupFromRelativePath(relative);
            grouped[group].emplace_back(
                RepoNameFromPath(repoPath),
                std::get<1>(stat),
                std::get<2>(stat));
        }

        std::cout << "\n=== Push Summary ===\n";
        std::cout << "SUMMARY: pushed_entries=" << pushStats.size() << ", groups=" << grouped.size() << "\n\n";

        std::size_t index = 0;
        for (const auto& [group, rows] : grouped) {
            std::cout << "GROUP: " << group << "\n";
            for (const auto& row : rows) {
                index += 1;
                std::cout << "[" << index << "] " << std::get<0>(row)
                          << "  remote=" << std::get<1>(row)
                          << "  branch=" << std::get<2>(row)
                          << "\n";
            }
            std::cout << "\n";
        }
    }

    std::cout << "\nSummary: " << successes << " succeeded, " << failures << " failed\n";
    if (InProfile) {
        const auto totalEnd = std::chrono::steady_clock::now();
        const auto totalMillis = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart).count();
        std::cout << "\n=== Profile Summary ===\n";
        std::cout << "mode: native\n";
        std::cout << "repo_count: " << InRepos.size() << "\n";
        std::cout << "jobs_requested: " << jobs << "\n";
        std::cout << "max_parallel_observed: " << maxParallelObserved << "\n";
        std::cout << "sync_ms: " << syncMillis << "\n";
        std::cout << "push_ms: " << pushMillis << "\n";
        std::cout << "total_ms: " << totalMillis << "\n";
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

auto RunPushNativeSimple(const std::filesystem::path& InWorkspaceRoot,
                         const bool InRecursive,
                         const bool InDryRun,
                         const bool InProfile,
                         const bool InForceWithLease,
                         const bool InNoVerify,
                         const int InJobs,
                         const bool InVerbose,
                         const std::string& InRemote) -> int {
    std::vector<std::filesystem::path> repos;
    if (InRecursive) {
        repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
        if (repos.empty()) {
            repos.push_back(InWorkspaceRoot);
        }
    } else {
        repos.push_back(InWorkspaceRoot);
    }

    return RunNativePush(
        repos,
        true,
        false,
        InDryRun,
        InForceWithLease,
        InNoVerify,
        true,
        false,
        InJobs,
        InProfile,
        InVerbose,
        InRemote);
}

void RegisterPush(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("push", "Multi-remote push workflow");
    cmd->allow_extras();

    auto* shellMode = new bool{false};
    auto* noRecursive = new bool{false};
    auto* repos = new std::string{};
    auto* skipSync = new bool{false};
    auto* fetchOnly = new bool{false};
    auto* dryRun = new bool{false};
    auto* forceWithLease = new bool{false};
    auto* noVerify = new bool{false};
    auto* noSmartSync = new bool{false};
    auto* stashLocalChanges = new bool{false};
    auto* failOnDirtySync = new bool{false};
    auto* jobs = new int{DetectDefaultPushJobs()};
    auto* profile = new bool{false};
    auto* verbose = new bool{false};
    auto* remote = new std::string{};

    cmd->add_flag("--shell", *shellMode, "Deprecated compatibility flag (shell path removed)");
    cmd->add_flag("--no-recursive,-N", *noRecursive, "Only push current repository (disable workspace recursive discovery)");
    cmd->add_flag("--current-only", *noRecursive, "Alias of --no-recursive (backward compatible)");
    cmd->add_option("--repos", *repos, "Repo filter (comma-separated paths, native mode)");
    cmd->add_flag("--skip-sync", *skipSync, "Skip sync step before push");
    cmd->add_flag("--fetch-only", *fetchOnly, "Run fetch only (skip rebase and push)");
    cmd->add_flag("--dry-run", *dryRun, "Preview push operations");
    cmd->add_flag("--force-with-lease", *forceWithLease, "Use force-with-lease for push");
    cmd->add_flag("--no-verify", *noVerify, "Pass --no-verify to git push");
    cmd->add_flag("--no-smart-sync", *noSmartSync, "Compatibility flag (native uses simple pull --rebase)");
    cmd->add_flag("--stash-local-changes", *stashLocalChanges, "Auto-stash local changes during native sync");
    cmd->add_flag("--fail-on-dirty-sync", *failOnDirtySync, "Fail native sync when local changes exist");
    cmd->add_option("--jobs", *jobs, "Number of parallel repo workers for native push (default: CPU cores)");
    cmd->add_flag("--profile", *profile, "Print native push timing/profile summary");
    cmd->add_flag("--verbose", *verbose, "Show detailed native push output including partial failures");
    cmd->add_option("--remote", *remote, "Native remote filter (default fan-out origin-ssh/http/origin)");

    cmd->callback([=]() {
        auto extras = cmd->remaining();

        if (*shellMode) {
            std::cerr << "Error: --shell is no longer supported; push workflow is fully native now\n";
            std::exit(2);
        }

        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native push mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        std::vector<std::filesystem::path> nativeRepos;
        if (!repos->empty()) {
            nativeRepos = ParseReposCsv(*repos);
        }

        if (nativeRepos.empty() && !*noRecursive) {
            nativeRepos = DiscoverWorkspaceRepos(std::filesystem::current_path());
        }

        if (nativeRepos.empty()) {
            nativeRepos.push_back(std::filesystem::current_path());
        }

        if (*stashLocalChanges && *failOnDirtySync) {
            std::cerr << "Error: --stash-local-changes and --fail-on-dirty-sync cannot be used together\n";
            std::exit(1);
        }

        if (*fetchOnly && *skipSync) {
            std::cerr << "Error: --fetch-only cannot be used with --skip-sync\n";
            std::exit(1);
        }

        if (*jobs < 1) {
            std::cerr << "Error: --jobs must be a positive integer\n";
            std::exit(1);
        }

        const auto code = RunNativePush(
            nativeRepos,
            *skipSync,
            *fetchOnly,
            *dryRun,
            *forceWithLease,
            *noVerify,
            *stashLocalChanges,
            *failOnDirtySync,
            *jobs,
            *profile,
            *verbose,
            *remote);
        std::exit(code);
    });
}

} // namespace kano::git::commands
