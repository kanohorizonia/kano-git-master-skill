// push command — Multi-remote push workflow
// Delegates to: scripts/commit-tools/smart-push.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <chrono>
#include <mutex>
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

auto RunNativePush(
    const std::vector<std::filesystem::path>& InRepos,
    const bool InSkipSync,
    const bool InSyncOnly,
    const bool InDryRun,
    const bool InForceWithLease,
    const bool InNoVerify,
    const bool InStashLocalChanges,
    const bool InFailOnDirtySync,
    const int InJobs,
    const bool InProfile,
    const std::string& InRemoteFilter) -> int {
    const auto totalStart = std::chrono::steady_clock::now();
    long long syncMillis = 0;
    long long pushMillis = 0;
    int maxParallelObserved = 1;
    std::mutex statsMutex;
    std::vector<std::tuple<std::string, std::string, std::string>> pushStats;
    int failures = 0;
    int successes = 0;

    auto runOneRepo = [&](const std::filesystem::path& repoPathRaw) -> std::pair<int, int> {
        const auto repoPath = std::filesystem::weakly_canonical(repoPathRaw);
        const auto repoLabel = repoPath.lexically_normal().generic_string();

        const auto gitDir = GitCapture(repoPath, {"rev-parse", "--git-dir"});
        if (gitDir.exitCode != 0) {
            std::cerr << "Error: Not a git repository: " << repoLabel << "\n";
            return {0, 1};
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
            } else if (InDryRun) {
                std::cout << "[DRY RUN] [" << repoLabel << "] Would run: git pull --rebase\n";
            } else {
                const auto pull = GitPassThrough(repoPath, {"pull", "--rebase"});
                if (pull.exitCode != 0) {
                    std::cerr << "[" << repoLabel << "] Sync failed before push\n";
                    return {0, 1};
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

        if (InSyncOnly) {
            std::cout << "[" << repoLabel << "] Sync-only mode: skipping push\n";
            return {1, 0};
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

            const auto result = GitPassThrough(repoPath, args);
            if (result.exitCode == 0) {
                std::cout << "[" << repoLabel << "] Pushed (" << remote << ")\n";
                repoSuccess = 1;
                {
                    std::lock_guard<std::mutex> lock(statsMutex);
                    pushStats.emplace_back(repoLabel, remote, branch);
                }
            } else {
                std::cerr << "[" << repoLabel << "] Push failed (" << remote << ")\n";
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
    if (jobs == 1 || InRepos.size() <= 1) {
        for (const auto& repoPathRaw : InRepos) {
            const auto [s, f] = runOneRepo(repoPathRaw);
            successes += s;
            failures += f;
        }
    } else {
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

        for (const auto& repoPathRaw : InRepos) {
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
        std::cout << "\n=== Push Summary ===\n";
        std::cout << std::left << std::setw(45) << "Repository"
                  << std::setw(20) << "Remote"
                  << "Branch\n";
        std::cout << std::left << std::setw(45) << "-----------"
                  << std::setw(20) << "------"
                  << "------\n";
        for (const auto& stat : pushStats) {
            std::cout << std::left << std::setw(45) << std::get<0>(stat)
                      << std::setw(20) << std::get<1>(stat)
                      << std::get<2>(stat) << "\n";
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

void RegisterPush(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("push", "Multi-remote push workflow");
    cmd->allow_extras();

    auto* shellMode = new bool{false};
    auto* repos = new std::string{};
    auto* skipSync = new bool{false};
    auto* syncOnly = new bool{false};
    auto* dryRun = new bool{false};
    auto* forceWithLease = new bool{false};
    auto* noVerify = new bool{false};
    auto* noSmartSync = new bool{false};
    auto* stashLocalChanges = new bool{false};
    auto* failOnDirtySync = new bool{false};
    auto* jobs = new int{1};
    auto* profile = new bool{false};
    auto* remote = new std::string{};

    cmd->add_flag("--shell", *shellMode, "Use shell fallback implementation");
    cmd->add_option("--repos", *repos, "Shell-mode repo filter (forces shell fallback)");
    cmd->add_flag("--skip-sync", *skipSync, "Skip sync step before push");
    cmd->add_flag("--sync-only", *syncOnly, "Run sync only and skip push");
    cmd->add_flag("--dry-run", *dryRun, "Preview push operations");
    cmd->add_flag("--force-with-lease", *forceWithLease, "Use force-with-lease for push");
    cmd->add_flag("--no-verify", *noVerify, "Pass --no-verify to git push");
    cmd->add_flag("--no-smart-sync", *noSmartSync, "Compatibility flag (native uses simple pull --rebase)");
    cmd->add_flag("--stash-local-changes", *stashLocalChanges, "Auto-stash local changes during native sync");
    cmd->add_flag("--fail-on-dirty-sync", *failOnDirtySync, "Fail native sync when local changes exist");
    cmd->add_option("--jobs", *jobs, "Number of parallel repo workers for native push");
    cmd->add_flag("--profile", *profile, "Print native push timing/profile summary");
    cmd->add_option("--remote", *remote, "Native remote filter (default fan-out origin-ssh/http/origin)");

    cmd->callback([=]() {
        auto extras = cmd->remaining();

        std::vector<std::filesystem::path> nativeRepos;
        if (!repos->empty()) {
            nativeRepos = ParseReposCsv(*repos);
        }
        if (nativeRepos.empty()) {
            nativeRepos.push_back(std::filesystem::current_path());
        }

        const bool forceShell = *shellMode || !extras.empty();
        if (forceShell) {
            std::vector<std::string> args;
            if (!repos->empty()) {
                args.push_back("--repos");
                args.push_back(*repos);
            }
            if (*skipSync) {
                args.push_back("--skip-sync");
            }
            if (*syncOnly) {
                args.push_back("--sync-only");
            }
            if (*dryRun) {
                args.push_back("--dry-run");
            }
            if (*forceWithLease) {
                args.push_back("--force-with-lease");
            }
            if (*noVerify) {
                args.push_back("--no-verify");
            }
            if (*noSmartSync) {
                args.push_back("--no-smart-sync");
            }
            if (*stashLocalChanges) {
                args.push_back("--stash-local-changes");
            }
            if (*failOnDirtySync) {
                args.push_back("--fail-on-dirty-sync");
            }
            if (*jobs > 1) {
                args.push_back("--jobs");
                args.push_back(std::to_string(*jobs));
            }
            if (*profile) {
                args.push_back("--profile");
            }
            args.insert(args.end(), extras.begin(), extras.end());
            auto result = shell::ExecuteScript("commit-tools/smart-push.sh", args);
            std::exit(result.exitCode);
        }

        if (*stashLocalChanges && *failOnDirtySync) {
            std::cerr << "Error: --stash-local-changes and --fail-on-dirty-sync cannot be used together\n";
            std::exit(1);
        }

        if (*jobs < 1) {
            std::cerr << "Error: --jobs must be a positive integer\n";
            std::exit(1);
        }

        const auto code = RunNativePush(
            nativeRepos,
            *skipSync,
            *syncOnly,
            *dryRun,
            *forceWithLease,
            *noVerify,
            *stashLocalChanges,
            *failOnDirtySync,
            *jobs,
            *profile,
            *remote);
        std::exit(code);
    });
}

} // namespace kano::git::commands
