// push command — Multi-remote push workflow
// Delegates to: scripts/commit-tools/smart-push.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <filesystem>
#include <iostream>
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

auto GitCapture(const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture);
}

auto GitPassThrough(const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::PassThrough);
}

auto HasRemote(const std::string& InRemote) -> bool {
    const auto out = GitCapture({"remote", "get-url", InRemote});
    return out.exitCode == 0;
}

auto RunNativePush(
    const bool InSkipSync,
    const bool InSyncOnly,
    const bool InDryRun,
    const bool InForceWithLease,
    const bool InNoVerify,
    const std::string& InRemoteFilter) -> int {
    const auto gitDir = GitCapture({"rev-parse", "--git-dir"});
    if (gitDir.exitCode != 0) {
        std::cerr << "Error: Not in a git repository\n";
        return 1;
    }

    const auto branchOut = GitCapture({"symbolic-ref", "--quiet", "--short", "HEAD"});
    const auto branch = Trim(branchOut.stdoutStr);
    if (branchOut.exitCode != 0 || branch.empty()) {
        std::cerr << "Error: Detached HEAD is not supported by native push flow\n";
        return 1;
    }

    const bool hasUpstream = (GitCapture({"rev-parse", "--abbrev-ref", "@{upstream}"}).exitCode == 0);
    const bool hasLocalChanges = !Trim(GitCapture({"status", "--porcelain"}).stdoutStr).empty();

    if (!InSkipSync && hasUpstream) {
        if (hasLocalChanges) {
            std::cout << "[.] Sync skipped: local changes present; proceeding to push\n";
        } else if (InDryRun) {
            std::cout << "[DRY RUN] Would run: git pull --rebase\n";
        } else {
            const auto pull = GitPassThrough({"pull", "--rebase"});
            if (pull.exitCode != 0) {
                std::cerr << "[.] Sync failed before push\n";
                return 1;
            }
        }
    }

    if (InSyncOnly) {
        std::cout << "[.] Sync-only mode: skipping push\n";
        return 0;
    }

    std::vector<std::string> pushRemotes;
    if (!InRemoteFilter.empty()) {
        if (HasRemote(InRemoteFilter)) {
            pushRemotes.push_back(InRemoteFilter);
        }
    } else {
        if (HasRemote("origin-ssh")) {
            pushRemotes.push_back("origin-ssh");
        }
        if (HasRemote("origin-http")) {
            pushRemotes.push_back("origin-http");
        }
        if (HasRemote("origin")) {
            pushRemotes.push_back("origin");
        }
    }

    if (pushRemotes.empty()) {
        std::cerr << "Error: No pushable origin remote found\n";
        return 1;
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

    int success = 0;
    for (const auto& remote : pushRemotes) {
        std::vector<std::string> args = {"push"};
        args.insert(args.end(), pushArgs.begin(), pushArgs.end());
        args.push_back(remote);
        args.push_back(branch);

        if (InDryRun) {
            std::cout << "[DRY RUN] Would run: git";
            for (const auto& arg : args) {
                std::cout << " " << arg;
            }
            std::cout << "\n";
            success = 1;
            continue;
        }

        const auto result = GitPassThrough(args);
        if (result.exitCode == 0) {
            std::cout << "[.] Pushed (" << remote << ")\n";
            success = 1;
        } else {
            std::cerr << "[.] Push failed (" << remote << ")\n";
        }
    }

    return success ? 0 : 1;
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

        const bool forceShell = *shellMode || !repos->empty() || !extras.empty();
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
