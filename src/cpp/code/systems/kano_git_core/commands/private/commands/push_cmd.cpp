// push command — Multi-remote push workflow
// Delegates to: scripts/commit-tools/smart-push.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
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
    const std::string& InRemoteFilter) -> int {
    int failures = 0;
    int successes = 0;

    for (const auto& repoPathRaw : InRepos) {
        const auto repoPath = std::filesystem::weakly_canonical(repoPathRaw);
        const auto repoLabel = repoPath.lexically_normal().generic_string();

        const auto gitDir = GitCapture(repoPath, {"rev-parse", "--git-dir"});
        if (gitDir.exitCode != 0) {
            std::cerr << "Error: Not a git repository: " << repoLabel << "\n";
            failures += 1;
            continue;
        }

        const auto branchOut = GitCapture(repoPath, {"symbolic-ref", "--quiet", "--short", "HEAD"});
        const auto branch = Trim(branchOut.stdoutStr);
        if (branchOut.exitCode != 0 || branch.empty()) {
            std::cerr << "Error: Detached HEAD is not supported by native push flow: " << repoLabel << "\n";
            failures += 1;
            continue;
        }

        const bool hasUpstream = (GitCapture(repoPath, {"rev-parse", "--abbrev-ref", "@{upstream}"}).exitCode == 0);
        const bool hasLocalChanges = !Trim(GitCapture(repoPath, {"status", "--porcelain"}).stdoutStr).empty();

        if (!InSkipSync && hasUpstream) {
            if (hasLocalChanges) {
                std::cout << "[" << repoLabel << "] Sync skipped: local changes present; proceeding to push\n";
            } else if (InDryRun) {
                std::cout << "[DRY RUN] [" << repoLabel << "] Would run: git pull --rebase\n";
            } else {
                const auto pull = GitPassThrough(repoPath, {"pull", "--rebase"});
                if (pull.exitCode != 0) {
                    std::cerr << "[" << repoLabel << "] Sync failed before push\n";
                    failures += 1;
                    continue;
                }
            }
        }

        if (InSyncOnly) {
            std::cout << "[" << repoLabel << "] Sync-only mode: skipping push\n";
            successes += 1;
            continue;
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
            failures += 1;
            continue;
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
                continue;
            }

            const auto result = GitPassThrough(repoPath, args);
            if (result.exitCode == 0) {
                std::cout << "[" << repoLabel << "] Pushed (" << remote << ")\n";
                repoSuccess = 1;
            } else {
                std::cerr << "[" << repoLabel << "] Push failed (" << remote << ")\n";
            }
        }

        if (repoSuccess == 0) {
            failures += 1;
        } else {
            successes += 1;
        }
    }

    std::cout << "Summary: " << successes << " succeeded, " << failures << " failed\n";
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
    auto* remote = new std::string{};

    cmd->add_flag("--shell", *shellMode, "Use shell fallback implementation");
    cmd->add_option("--repos", *repos, "Shell-mode repo filter (forces shell fallback)");
    cmd->add_flag("--skip-sync", *skipSync, "Skip sync step before push");
    cmd->add_flag("--sync-only", *syncOnly, "Run sync only and skip push");
    cmd->add_flag("--dry-run", *dryRun, "Preview push operations");
    cmd->add_flag("--force-with-lease", *forceWithLease, "Use force-with-lease for push");
    cmd->add_flag("--no-verify", *noVerify, "Pass --no-verify to git push");
    cmd->add_flag("--no-smart-sync", *noSmartSync, "Compatibility flag (native uses simple pull --rebase)");
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
            args.insert(args.end(), extras.begin(), extras.end());
            auto result = shell::ExecuteScript("commit-tools/smart-push.sh", args);
            std::exit(result.exitCode);
        }

        const auto code = RunNativePush(
            nativeRepos,
            *skipSync,
            *syncOnly,
            *dryRun,
            *forceWithLease,
            *noVerify,
            *remote);
        std::exit(code);
    });
}

} // namespace kano::git::commands
